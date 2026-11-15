"""Check SHM imports, independent Render pixels, and scoped protocol errors."""

import os
from pathlib import Path
import struct
import sys
import tempfile
import time

from host_support import connect_x11, receive_exact, server, STARTUP_TIMEOUT
from native_lifecycle import wait_socket, POLL_INTERVAL
from native_surfaces import root_window, query_tree, create_surface, WL_SURFACE_ATTACH, WL_SURFACE_COMMIT, WL_SURFACE_DESTROY, WL_SURFACE_FRAME
from wayland_support import Wayland, WaylandProtocolError

POOL_CREATE_BUFFER = 0
POOL_DESTROY = 1
POOL_RESIZE = 2
BUFFER_DESTROY = 0
MAP_WINDOW = 8
CLEAR_AREA = 61
GET_IMAGE = 73
Z_PIXMAP = 2
X11_REPLY_SIZE = 32
BYTES_PER_PIXEL = 4
WIDTH = 2
HEIGHT = 2
NORMAL_STRIDE = 12
LARGE_STRIDE = 1 << 16
BUFFER_OFFSET = 4
COLORS = (0xff123456, 0xffabcdef, 0xff654321, 0xfffedcba)
RGB_MASK = 0xffffff
SHM_ARGB8888 = 0
SHM_XRGB8888 = 1
SHM_INVALID_FORMAT = 0
SHM_INVALID_STRIDE = 1
SHM_INVALID_FD = 2


def create_pool(client, shm, fd, size):
    pool = client.allocate_id()
    client.send(shm, 0, struct.pack("=Ii", pool, size), [fd])
    return pool


def create_buffer(client, pool, offset, width, height, stride, format):
    buffer = client.allocate_id()
    client.send(pool, POOL_CREATE_BUFFER, struct.pack("=IiiiiI", buffer, offset,
                                                     width, height, stride, format))
    return buffer


def get_image(x11, window):
    x11.sendall(struct.pack("<BBHIhhHHI", GET_IMAGE, Z_PIXMAP, 5, window,
                            0, 0, WIDTH, HEIGHT, 0xffffffff))
    reply = receive_exact(x11, X11_REPLY_SIZE)
    assert reply[0] == 1, reply
    length, = struct.unpack_from("<I", reply, 4)
    pixels = receive_exact(x11, length * 4)
    return tuple(value & RGB_MASK for value in struct.unpack(f"={WIDTH * HEIGHT}I", pixels))


def wait_pixels(x11, window, expected_colors=COLORS):
    expected = tuple(value & RGB_MASK for value in expected_colors)
    deadline = time.monotonic() + STARTUP_TIMEOUT
    while get_image(x11, window) != expected:
        assert time.monotonic() < deadline, "Native Render pixel check timed out"
        time.sleep(POLL_INTERVAL)


def check_buffer(x11, root, path, stride, format, destroy_before_commit=False, fault=False):
    client = Wayland(path)
    fd = os.memfd_create("meatball-shm-test", os.MFD_CLOEXEC)
    try:
        compositor = client.bind("wl_compositor")
        shm = client.bind("wl_shm")
        formats = {struct.unpack("=I", data)[0] for object_id, opcode, data in client.roundtrip_events()
                   if object_id == shm and opcode == 0}
        assert formats == {SHM_ARGB8888, SHM_XRGB8888}
        before = query_tree(x11, root)
        surface = create_surface(client, compositor)
        window, = query_tree(x11, root) - before
        size = BUFFER_OFFSET + stride * HEIGHT
        os.ftruncate(fd, size)
        for row in range(HEIGHT):
            os.pwrite(fd, struct.pack("=II", *COLORS[row * WIDTH:(row + 1) * WIDTH]),
                      BUFFER_OFFSET + row * stride)
        pool = create_pool(client, shm, fd, size)
        buffer = create_buffer(client, pool, BUFFER_OFFSET, WIDTH, HEIGHT, stride, format)
        client.send(pool, POOL_DESTROY)
        client.send(surface, WL_SURFACE_ATTACH, struct.pack("=Iii", buffer, 0, 0))
        if destroy_before_commit:
            client.send(buffer, BUFFER_DESTROY)
        client.send(surface, WL_SURFACE_COMMIT)
        events = client.roundtrip_events()
        assert ((buffer, 0, b"") in events) != destroy_before_commit
        os.pwrite(fd, bytes(size), 0)
        if not destroy_before_commit:
            client.send(buffer, BUFFER_DESTROY)
        client.sync()
        x11.sendall(struct.pack("<BBHI", MAP_WINDOW, 0, 2, window))
        wait_pixels(x11, window, (0,) * len(COLORS) if fault else COLORS)
        callback = client.allocate_id()
        client.send(surface, WL_SURFACE_FRAME, struct.pack("=I", callback))
        client.send(surface, WL_SURFACE_COMMIT)
        while True:
            object_id, opcode, data = client.receive()
            if object_id == callback:
                assert opcode == 0 and len(data) == BYTES_PER_PIXEL
                break
        x11.sendall(struct.pack("<BBHIhhHH", CLEAR_AREA, 1, 4, window,
                                0, 0, WIDTH, HEIGHT))
        wait_pixels(x11, window, (0,) * len(COLORS) if fault else COLORS)
        callback = client.allocate_id()
        client.send(surface, WL_SURFACE_FRAME, struct.pack("=I", callback))
        client.send(surface, WL_SURFACE_COMMIT)
        client.send(surface, WL_SURFACE_DESTROY)
        client.sync()
        assert query_tree(x11, root) == before
    finally:
        os.close(fd)
        client.close()


def check_pool_resize(path):
    client = Wayland(path)
    fd = os.memfd_create("meatball-shm-resize", os.MFD_CLOEXEC)
    try:
        shm = client.bind("wl_shm")
        os.ftruncate(fd, NORMAL_STRIDE)
        pool = create_pool(client, shm, fd, NORMAL_STRIDE)
        os.ftruncate(fd, NORMAL_STRIDE * HEIGHT)
        client.send(pool, POOL_RESIZE, struct.pack("=i", NORMAL_STRIDE * HEIGHT))
        create_buffer(client, pool, 0, WIDTH, HEIGHT, NORMAL_STRIDE, SHM_XRGB8888)
        client.sync()
    finally:
        os.close(fd)
        client.close()


def check_invalid(path):
    cases = [
        (-1, WIDTH, HEIGHT, NORMAL_STRIDE, SHM_XRGB8888, SHM_INVALID_STRIDE),
        (0, -1, HEIGHT, NORMAL_STRIDE, SHM_XRGB8888, SHM_INVALID_STRIDE),
        (0, WIDTH, HEIGHT, BYTES_PER_PIXEL, SHM_XRGB8888, SHM_INVALID_STRIDE),
        (NORMAL_STRIDE, WIDTH, HEIGHT, NORMAL_STRIDE, SHM_XRGB8888, SHM_INVALID_STRIDE),
        (0, WIDTH, HEIGHT, NORMAL_STRIDE, 0xffffffff, SHM_INVALID_FORMAT),
    ]
    observer = Wayland(path)
    try:
        for pool_size, file_size, readonly in [(0, NORMAL_STRIDE, False),
                                                (NORMAL_STRIDE * HEIGHT, NORMAL_STRIDE, False),
                                                (NORMAL_STRIDE, NORMAL_STRIDE, True)]:
            client = Wayland(path)
            fd = os.memfd_create("meatball-shm-invalid-pool", os.MFD_CLOEXEC)
            sent_fd = fd
            try:
                shm = client.bind("wl_shm")
                os.ftruncate(fd, file_size)
                if readonly:
                    sent_fd = os.open(f"/proc/self/fd/{fd}", os.O_RDONLY | os.O_CLOEXEC)
                create_pool(client, shm, sent_fd, pool_size)
                try:
                    client.sync()
                except WaylandProtocolError as error:
                    assert error.object_id == shm and error.code == SHM_INVALID_FD
                else:
                    raise AssertionError("Invalid SHM pool was accepted")
            finally:
                if sent_fd != fd:
                    os.close(sent_fd)
                os.close(fd)
                client.close()
            observer.sync()
        for offset, width, height, stride, format, code in cases:
            client = Wayland(path)
            fd = os.memfd_create("meatball-shm-invalid", os.MFD_CLOEXEC)
            try:
                shm = client.bind("wl_shm")
                os.ftruncate(fd, NORMAL_STRIDE * HEIGHT)
                pool = create_pool(client, shm, fd, NORMAL_STRIDE * HEIGHT)
                create_buffer(client, pool, offset, width, height, stride, format)
                try:
                    client.sync()
                except WaylandProtocolError as error:
                    assert error.object_id == pool and error.code == code
                else:
                    raise AssertionError("Invalid SHM buffer was accepted")
            finally:
                os.close(fd)
                client.close()
            observer.sync()
    finally:
        observer.close()


def main():
    host, module = sys.argv[1:3]
    with tempfile.TemporaryDirectory(prefix="meatball-shm-") as directory:
        path = Path(directory) / "wayland"
        with server(host, module, {"MEATBALL_TEST_SOCKET": str(path)}) as (process, display):
            x11, setup = connect_x11(display, return_setup=True)
            try:
                wait_socket(path, process)
                root = root_window(setup)
                if "--fault" in sys.argv:
                    check_buffer(x11, root, path, NORMAL_STRIDE, SHM_XRGB8888, fault=True)
                    observer = Wayland(path)
                    observer.sync()
                    observer.close()
                    return
                for format in [SHM_ARGB8888, SHM_XRGB8888]:
                    check_buffer(x11, root, path, NORMAL_STRIDE, format)
                check_buffer(x11, root, path, LARGE_STRIDE, SHM_XRGB8888, True)
                check_pool_resize(path)
                check_invalid(path)
            finally:
                x11.close()


if __name__ == "__main__":
    main()
