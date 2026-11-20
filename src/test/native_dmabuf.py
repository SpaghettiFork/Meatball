"""Check linux-dmabuf with the isolated X server's test-only DRI3 driver."""

import fcntl
import mmap
import os
from pathlib import Path
import struct
import sys
import tempfile
import time

from host_support import STARTUP_TIMEOUT, connect_x11, server
from native_lifecycle import POLL_INTERVAL, wait_socket
from native_shm import (COLORS, HEIGHT, WIDTH, BYTES_PER_PIXEL, wait_pixels,
                        RGB565_PIXELS, RGB16_EXPECTED, RGB16_BYTES)
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
INVALID_FORMAT = 4
INVALID_WL_BUFFER = 7
Y_INVERT = 1
INTERLACED = 2
RGB = int.from_bytes(b"XR24", "little")
ARGB = int.from_bytes(b"AR24", "little")
RGB565 = int.from_bytes(b"RG16", "little")
LINEAR = 0
IMPLICIT = (1 << 56) - 1
FORMAT_MODIFIER_VERSION = 3
FEEDBACK_VERSION = 4
DEFAULT_FEEDBACK = 2
SURFACE_FEEDBACK = 3
FEEDBACK_DESTROY = 0
FEEDBACK_DONE = 0
FEEDBACK_TABLE = 1
FEEDBACK_MAIN_DEVICE = 2
FEEDBACK_TRANCHE_DONE = 3
FEEDBACK_TARGET_DEVICE = 4
FEEDBACK_FORMATS = 5
FEEDBACK_FLAGS = 6
FEEDBACK_ENTRY = struct.Struct("=IIQ")
TEST_DEVICE = "/dev/null"
ADVERTISED_PAIRS = {(RGB, LINEAR), (ARGB, LINEAR), (RGB565, LINEAR), (RGB, IMPLICIT)}
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


def check_feedback(client, manager):
    events = client.roundtrip_events()
    assert not any(obj == manager for obj, _, _ in events)
    for request, args in ((DEFAULT_FEEDBACK, ()), (SURFACE_FEEDBACK, (client.surface,))):
        feedback = client.allocate_id()
        client.send(manager, request, struct.pack(f"={1 + len(args)}I", feedback, *args))
        events = [(opcode, data) for obj, opcode, data in client.roundtrip_events()
                  if obj == feedback]
        assert [opcode for opcode, _ in events] == [
            FEEDBACK_TABLE, FEEDBACK_MAIN_DEVICE, FEEDBACK_TARGET_DEVICE,
            FEEDBACK_FLAGS, FEEDBACK_FORMATS, FEEDBACK_TRANCHE_DONE, FEEDBACK_DONE]
        size, = struct.unpack("=I", events[0][1])
        fd = client.fds.pop(0)
        try:
            assert size == len(ADVERTISED_PAIRS) * FEEDBACK_ENTRY.size
            seals = fcntl.F_SEAL_WRITE | fcntl.F_SEAL_SHRINK | fcntl.F_SEAL_GROW
            assert fcntl.fcntl(fd, fcntl.F_GET_SEALS) & seals == seals
            with mmap.mmap(fd, size, flags=mmap.MAP_PRIVATE, prot=mmap.PROT_READ) as table:
                entries = []
                for format, padding, modifier in FEEDBACK_ENTRY.iter_unpack(table):
                    assert padding == 0
                    entries.append((format, modifier))
            device = os.stat(TEST_DEVICE).st_rdev
            for _, payload in events[1:3]:
                length, = struct.unpack_from("=I", payload)
                assert length == struct.calcsize("@Q")
                assert int.from_bytes(payload[4:4 + length], sys.byteorder) == device
            assert events[3][1] == struct.pack("=I", 0)
            indices = events[4][1]
            byte_count, = struct.unpack_from("=I", indices)
            assert byte_count == len(entries) * struct.calcsize("=H")
            indices = struct.unpack_from(f"={len(entries)}H", indices, 4)
            assert {entries[i] for i in indices} == ADVERTISED_PAIRS
        finally:
            os.close(fd)
        client.send(feedback, FEEDBACK_DESTROY)
    client.sync()


def check_import(path, display, fd, version):
    observer, setup = connect_x11(display, return_setup=True)
    root = root_window(setup)
    before = query_tree(observer, root)
    client = XdgClient(path)
    try:
        window, = query_tree(observer, root) - before
        deadline = time.monotonic() + STARTUP_TIMEOUT
        while client.globals()[INTERFACE][1] != version:
            assert time.monotonic() < deadline, "DMA-BUF feedback version did not settle"
            time.sleep(POLL_INTERVAL)
        manager = client.bind(INTERFACE, version)
        if version >= FEEDBACK_VERSION:
            check_feedback(client, manager)
            modifier_manager = client.bind(INTERFACE, FORMAT_MODIFIER_VERSION)
        else:
            modifier_manager = manager
        events = client.roundtrip_events()
        advertised = set()
        for obj, opcode, data in events:
            if obj == modifier_manager and opcode == MODIFIER:
                format, high, low = struct.unpack("=III", data)
                advertised.add((format, high << 32 | low))
        assert advertised == ADVERTISED_PAIRS
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

        os.pwrite(fd, struct.pack(f"={len(RGB565_PIXELS)}H", *RGB565_PIXELS), OFFSET)
        params = new_params(client, manager)
        add(client, params, fd, stride=WIDTH * RGB16_BYTES)
        buffer = client.allocate_id()
        client.send(params, PARAMS_CREATE_IMMED,
                    struct.pack("=IiiII", buffer, WIDTH, HEIGHT, RGB565, 0))
        client.send(params, PARAMS_DESTROY)
        frame = client.frame()
        client.attach(buffer)
        client.commit()
        buffer_release(client, buffer)
        client.frame_done(frame)
        wait_pixels(observer, window, RGB16_EXPECTED)
    finally:
        client.close()
        observer.close()


def check_failures(path, fd, version):
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
            ("modifier", INVALID_FORMAT if version >= FEEDBACK_VERSION
                         else INVALID_WL_BUFFER)):
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
            for mode, version in (("no-device", FORMAT_MODIFIER_VERSION),
                                  ("1", FEEDBACK_VERSION), ("async", FEEDBACK_VERSION)):
                os.pwrite(fd, struct.pack(f"={len(COLORS)}I", *COLORS), OFFSET)
                with server(host, module, {"MEATBALL_TEST_SOCKET": str(path),
                                           "MEATBALL_TEST_DRI3": mode},
                            arguments=["-pixdepths", "16"]) as (process, display):
                    wait_socket(path, process)
                    check_import(path, display, fd, version)
                    check_failures(path, fd, version)
                    assert process.poll() is None
        finally:
            os.close(fd)


if __name__ == "__main__":
    main()
