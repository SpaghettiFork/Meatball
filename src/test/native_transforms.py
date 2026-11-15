"""Check logical surface sizes, all buffer orientations, and committed state."""

import os
from pathlib import Path
import struct
import sys
import tempfile
import time

from host_support import connect_x11, receive_exact, server, STARTUP_TIMEOUT
from native_lifecycle import wait_socket, POLL_INTERVAL
from native_shm import create_pool, create_buffer, SHM_ARGB8888
from native_surfaces import root_window, query_tree, create_surface
from native_xdg import XdgClient
from wayland_support import Wayland, WaylandProtocolError

GET_GEOMETRY = 14
GET_IMAGE = 73
X11_REPLY_SIZE = 32
Z_PIXMAP = 2
BYTES_PER_PIXEL = 4
SURFACE_ATTACH = 1
SURFACE_DAMAGE = 2
SURFACE_COMMIT = 6
SURFACE_TRANSFORM = 7
SURFACE_SCALE = 8
SURFACE_DAMAGE_BUFFER = 9
SURFACE_INVALID_SCALE = 0
SURFACE_INVALID_TRANSFORM = 1
SURFACE_INVALID_SIZE = 2
BUFFER_DESTROY = 0
POOL_DESTROY = 1
NORMAL = 0
ROTATION_COUNT = 4
TRANSFORM_COUNT = 8
FLIPPED = 4
PIXELS = ((0xff203040, 0xff405060, 0xff607080),
          (0xff8090a0, 0xffa0b0c0, 0xffc0d0e0))


def dimensions(x11, window):
    x11.sendall(struct.pack("<BBHI", GET_GEOMETRY, 0, 2, window))
    reply = receive_exact(x11, X11_REPLY_SIZE)
    assert reply[0] == 1, reply
    return struct.unpack_from("<HH", reply, 16)


def image(x11, window, width, height):
    x11.sendall(struct.pack("<BBHIhhHHI", GET_IMAGE, Z_PIXMAP, 5, window,
                            0, 0, width, height, 0xffffffff))
    reply = receive_exact(x11, X11_REPLY_SIZE)
    assert reply[0] == 1, reply
    length, = struct.unpack_from("<I", reply, 4)
    data = receive_exact(x11, length * BYTES_PER_PIXEL)
    return struct.unpack(f"={width * height}I", data)


def wait_image(x11, window, rows):
    height, width = len(rows), len(rows[0])
    expected = tuple(value for row in rows for value in row)
    deadline = time.monotonic() + STARTUP_TIMEOUT
    while True:
        if dimensions(x11, window) == (width, height):
            actual = image(x11, window, width, height)
            if actual == expected:
                return
        assert time.monotonic() < deadline, (width, height, expected)
        time.sleep(POLL_INTERVAL)


def transformed(rows, transform, scale=1):
    result = [tuple(pixel for value in row for pixel in [value] * scale)
              for row in rows for _ in range(scale)]
    if transform >= FLIPPED:
        result = [row[::-1] for row in result]
    for _ in range(transform % ROTATION_COUNT):
        result = list(zip(*result))[::-1]
    return result


def attach_rows(client, rows):
    width, height = len(rows[0]), len(rows)
    fd = os.memfd_create("meatball-transform-test", os.MFD_CLOEXEC)
    stride = width * BYTES_PER_PIXEL
    os.ftruncate(fd, stride * height)
    data = tuple(value for row in rows for value in row)
    os.pwrite(fd, struct.pack(f"={len(data)}I", *data), 0)
    pool = create_pool(client, client.shm, fd, stride * height)
    buffer = create_buffer(client, pool, 0, width, height, stride, SHM_ARGB8888)
    client.send(pool, POOL_DESTROY)
    client.attach(buffer)
    return fd, buffer


def check_transforms(path, x11, root):
    before = query_tree(x11, root)
    client = XdgClient(path)
    try:
        window, = query_tree(x11, root) - before
        client.commit()
        client.configure()
        for transform in range(TRANSFORM_COUNT):
            for scale in (1, 2, 3):
                client.send(client.surface, SURFACE_TRANSFORM, struct.pack("=i", transform))
                client.send(client.surface, SURFACE_SCALE, struct.pack("=i", scale))
                fd, buffer = attach_rows(client, transformed(PIXELS, transform, scale))
                try:
                    client.commit()
                    assert (buffer, 0, b"") in client.roundtrip_events()
                    wait_image(x11, window, PIXELS)
                    # Released client storage can change before a damage-only commit.
                    os.pwrite(fd, bytes(os.fstat(fd).st_size), 0)
                    client.send(client.surface, SURFACE_DAMAGE_BUFFER,
                                struct.pack("=iiii", 0, 0, 1, 1))
                    client.commit()
                    assert (buffer, 0, b"") not in client.roundtrip_events()
                    wait_image(x11, window, PIXELS)
                    client.send(buffer, BUFFER_DESTROY)
                finally:
                    os.close(fd)
        # Scale and transform changes reuse the retained snapshot after buffer destruction.
        client.send(client.surface, SURFACE_SCALE, struct.pack("=i", 1))
        client.send(client.surface, SURFACE_TRANSFORM, struct.pack("=i", NORMAL))
        client.sync()
        wait_image(x11, window, PIXELS)
        client.commit()
        client.sync()
        wait_image(x11, window, transformed(PIXELS, TRANSFORM_COUNT - 1, 3))
    finally:
        client.close()


def check_error(path, opcode, value, code, with_buffer=False):
    client = Wayland(path)
    try:
        compositor = client.bind("wl_compositor")
        surface = create_surface(client, compositor)
        if with_buffer:
            shm = client.bind("wl_shm")
            fd = os.memfd_create("meatball-invalid-scale", os.MFD_CLOEXEC)
            try:
                size = len(PIXELS) * len(PIXELS[0]) * BYTES_PER_PIXEL
                os.ftruncate(fd, size)
                pool = create_pool(client, shm, fd, size)
                buffer = create_buffer(client, pool, 0, len(PIXELS[0]), len(PIXELS),
                                       len(PIXELS[0]) * BYTES_PER_PIXEL, SHM_ARGB8888)
                client.send(surface, SURFACE_ATTACH, struct.pack("=Iii", buffer, 0, 0))
            finally:
                os.close(fd)
        client.send(surface, opcode, struct.pack("=i", value))
        client.send(surface, SURFACE_COMMIT)
        try:
            client.sync()
            raise AssertionError("invalid surface state was accepted")
        except WaylandProtocolError as error:
            assert (error.object_id, error.code) == (surface, code), str(error)
    finally:
        client.close()


def main():
    host, module = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="meatball-transforms-") as directory:
        path = Path(directory) / "wayland-native"
        with server(host, module, {"MEATBALL_TEST_SOCKET": str(path)}) as (process, display):
            wait_socket(path, process)
            x11, setup = connect_x11(display, return_setup=True)
            try:
                check_transforms(path, x11, root_window(setup))
                for scale in (0, -1):
                    check_error(path, SURFACE_SCALE, scale, SURFACE_INVALID_SCALE)
                for transform in (-1, TRANSFORM_COUNT):
                    check_error(path, SURFACE_TRANSFORM, transform, SURFACE_INVALID_TRANSFORM)
                check_error(path, SURFACE_SCALE, 2, SURFACE_INVALID_SIZE, with_buffer=True)
                observer = Wayland(path)
                try:
                    for version in range(1, 5):
                        compositor = observer.bind("wl_compositor", version)
                        surface = create_surface(observer, compositor)
                        observer.send(surface, SURFACE_COMMIT)
                    observer.sync()
                finally:
                    observer.close()
                assert process.poll() is None
            finally:
                x11.close()


if __name__ == "__main__":
    main()
