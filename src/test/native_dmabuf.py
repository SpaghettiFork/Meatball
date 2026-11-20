"""Check linux-dmabuf with the isolated X server's test-only DRI3 driver."""

import os
from pathlib import Path
import struct
import sys
import tempfile

from host_support import connect_x11, server
from native_lifecycle import wait_socket
from native_shm import COLORS, HEIGHT, WIDTH, BYTES_PER_PIXEL, wait_pixels
from native_surfaces import query_tree, root_window
from native_xdg import XdgClient
from wayland_support import Wayland, WaylandProtocolError

INTERFACE = "zwp_linux_dmabuf_v1"
CREATE_PARAMS = 1
FORMAT = 0
MODIFIER = 1
PARAMS_DESTROY = 0
PARAMS_ADD = 1
PARAMS_CREATE = 2
PARAMS_CREATE_IMMED = 3
CREATED = 0
FAILED = 1
BUFFER_DESTROY = 0
BUFFER_RELEASE = 0
ALREADY_USED = 0
PLANE_IDX = 1
PLANE_SET = 2
INCOMPLETE = 3
INVALID_DIMENSIONS = 5
OUT_OF_BOUNDS = 6
INVALID_WL_BUFFER = 7
Y_INVERT = 1
INTERLACED = 2
RGB = int.from_bytes(b"XR24", "little")
ARGB = int.from_bytes(b"AR24", "little")
LINEAR = 0
IMPLICIT = (1 << 56) - 1
FORMAT_MODIFIER_VERSION = 3
STRIDE = WIDTH * BYTES_PER_PIXEL
OFFSET = BYTES_PER_PIXEL
SIZE = OFFSET + STRIDE * HEIGHT


def new_params(client, manager):
    params = client.allocate_id()
    client.send(manager, CREATE_PARAMS, struct.pack("=I", params))
    return params


def add(client, params, fd, index=0, offset=OFFSET, stride=STRIDE,
        modifier=LINEAR):
    client.send(params, PARAMS_ADD,
                struct.pack("=IIIII", index, offset, stride, modifier >> 32,
                            modifier & 0xffffffff), fds=[fd])


def params_event(client, params, expected):
    while True:
        resource, opcode, data = client.receive()
        if resource == params:
            assert opcode == expected
            return data


def buffer_release(client, buffer):
    while True:
        resource, opcode, _ = client.receive()
        if resource == buffer:
            assert opcode == BUFFER_RELEASE
            return


def check_import(path, display, fd):
    observer, setup = connect_x11(display, return_setup=True)
    root = root_window(setup)
    before = query_tree(observer, root)
    client = XdgClient(path)
    try:
        window, = query_tree(observer, root) - before
        assert client.globals()[INTERFACE][1] == FORMAT_MODIFIER_VERSION
        manager = client.bind(INTERFACE)
        events = client.roundtrip_events()
        assert {struct.unpack("=III", data) for obj, opcode, data in events
                if obj == manager and opcode == MODIFIER} == {
                    (RGB, 0, LINEAR), (ARGB, 0, LINEAR),
                    (RGB, IMPLICIT >> 32, IMPLICIT & 0xffffffff)}
        legacy = client.bind(INTERFACE, 1)
        formats = [struct.unpack("=I", data)[0]
                   for obj, opcode, data in client.roundtrip_events()
                   if obj == legacy and opcode == FORMAT]
        assert formats == [RGB]

        client.commit()
        client.configure()
        params = new_params(client, manager)
        add(client, params, fd)
        buffer = client.allocate_id()
        client.send(params, PARAMS_CREATE_IMMED,
                    struct.pack("=IiiII", buffer, WIDTH, HEIGHT, RGB, 0))
        client.send(params, PARAMS_DESTROY)
        frame = client.frame()
        client.attach(buffer)
        client.commit()
        buffer_release(client, buffer)
        client.frame_done(frame)
        wait_pixels(observer, window, COLORS)

        changed = COLORS[::-1]
        os.pwrite(fd, struct.pack(f"={len(changed)}I", *changed), OFFSET)
        # The released client's pixels can change without changing the scene.
        frame = client.frame()
        client.commit()
        client.frame_done(frame)
        wait_pixels(observer, window, COLORS)
        frame = client.frame()
        client.attach(buffer)
        client.send(buffer, BUFFER_DESTROY)
        client.commit()
        client.frame_done(frame)
        wait_pixels(observer, window, changed)

        params = new_params(client, manager)
        add(client, params, fd)
        client.send(params, PARAMS_CREATE,
                    struct.pack("=iiII", WIDTH, HEIGHT, ARGB, Y_INVERT))
        buffer, = struct.unpack("=I", params_event(client, params, CREATED))
        client.send(params, PARAMS_DESTROY)
        frame = client.frame()
        client.attach(buffer)
        client.commit()
        buffer_release(client, buffer)
        client.frame_done(frame)
        # This time the source buffer carries alpha.
        flipped = changed[WIDTH:] + changed[:WIDTH]
        wait_pixels(observer, window, flipped)
    finally:
        client.close()
        observer.close()


def check_failures(path, fd):
    client = Wayland(path)
    try:
        manager = client.bind(INTERFACE)
        params = new_params(client, manager)
        add(client, params, fd)
        client.send(params, PARAMS_CREATE,
                    struct.pack("=iiII", WIDTH, HEIGHT, RGB, INTERLACED))
        assert params_event(client, params, FAILED) == b""
        client.send(params, PARAMS_DESTROY)
        client.sync()
    finally:
        client.close()

    for case, expected in (
            ("used", ALREADY_USED), ("index", PLANE_IDX),
            ("duplicate", PLANE_SET), ("missing", INCOMPLETE),
            ("extent", OUT_OF_BOUNDS), ("dimensions", INVALID_DIMENSIONS),
            ("modifier", INVALID_WL_BUFFER)):
        client = Wayland(path)
        try:
            manager = client.bind(INTERFACE)
            params = new_params(client, manager)
            if case == "index":
                add(client, params, fd, index=4)
            elif case != "missing":
                add(client, params, fd, offset=SIZE if case == "extent" else OFFSET,
                    modifier=IMPLICIT if case == "modifier" else LINEAR)
            if case == "duplicate":
                add(client, params, fd)
            width = 0 if case == "dimensions" else WIDTH
            format = ARGB if case == "modifier" else RGB
            if case not in ("index", "duplicate"):
                if case == "used":
                    client.send(params, PARAMS_CREATE,
                                struct.pack("=iiII", width, HEIGHT, format, 0))
                buffer = client.allocate_id()
                client.send(params, PARAMS_CREATE_IMMED,
                            struct.pack("=IiiII", buffer, width, HEIGHT, format, 0))
            try:
                client.sync()
            except WaylandProtocolError as error:
                assert (error.object_id, error.code) == (params, expected), error
            else:
                raise AssertionError(f"DMA-BUF accepted {case}")
        finally:
            client.close()


def main():
    host, module = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="meatball-dmabuf-") as directory:
        path = Path(directory) / "wayland"
        with server(host, module, {"MEATBALL_TEST_SOCKET": str(path)}) as (process, _):
            wait_socket(path, process)
            client = Wayland(path)
            try:
                assert INTERFACE not in client.globals()
            finally:
                client.close()
        fd = os.memfd_create("meatball-dmabuf-test", os.MFD_CLOEXEC)
        try:
            os.ftruncate(fd, SIZE)
            os.pwrite(fd, struct.pack(f"={len(COLORS)}I", *COLORS), OFFSET)
            with server(host, module, {"MEATBALL_TEST_SOCKET": str(path),
                                       "MEATBALL_TEST_DRI3": "1"}) as (process, display):
                wait_socket(path, process)
                check_import(path, display, fd)
                check_failures(path, fd)
                assert process.poll() is None
        finally:
            os.close(fd)


if __name__ == "__main__":
    main()
