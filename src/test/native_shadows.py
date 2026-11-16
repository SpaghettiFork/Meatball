"""Check client shadow margins on desktops with and without a compositor."""

import os
from pathlib import Path
import signal
import struct
import sys
import tempfile
import time

from host_support import connect_x11, server, STARTUP_TIMEOUT
from native_clients import intern_atom, property32, XA_CARDINAL
from native_lifecycle import wait_socket, POLL_INTERVAL
from native_surfaces import root_window, query_tree, shape_rectangles, SHAPE_BOUNDING
from native_transforms import attach_rows, image, transformed, SURFACE_SCALE, SURFACE_TRANSFORM
from native_xdg import (XdgClient, XDG_GEOMETRY, TOP_DESTROY, XDG_DESTROY,
                        XA_WM_SIZE_HINTS)

CHANGE_ATTRIBUTES = 2
CREATE_WINDOW = 1
DESTROY_WINDOW = 4
SET_SELECTION_OWNER = 22
CLEAR_AREA = 61
CW_BACKGROUND_PIXEL = 1 << 1
INPUT_ONLY = 2
COPY_FROM_PARENT = 0
CURRENT_TIME = 0
RESOURCE_BASE_OFFSET = 4
DESKTOP_COLOR = 0x426e37
WINDOW_COLOR = 0xffc08040
TRANSPARENT = 0
SHADOW = 0x40000000
RGB_MASK = 0xffffff
WIDTH = 12
HEIGHT = 10
GEOMETRY = (3, 2, 6, 5)
FULL_BOUNDS = [(0, 0, WIDTH, HEIGHT)]
GEOMETRY_BOUNDS = [GEOMETRY]
HINT_MIN_SIZE = 1 << 4
HINT_RESIZE_INC = 1 << 6
HINT_BASE_SIZE = 1 << 8
HINT_INCREMENTS = slice(9, 11)
HINT_BASE = slice(15, 17)


def wait_shape(x11, window, expected):
    deadline = time.monotonic() + STARTUP_TIMEOUT
    while True:
        actual = shape_rectangles(x11, window, SHAPE_BOUNDING)
        if actual == expected:
            return
        assert time.monotonic() < deadline, (actual, expected)
        time.sleep(POLL_INTERVAL)


def wait_desktop(x11, root, expected):
    deadline = time.monotonic() + STARTUP_TIMEOUT
    while True:
        actual = tuple(pixel & RGB_MASK for pixel in image(x11, root, WIDTH, HEIGHT))
        if actual == expected:
            return
        assert time.monotonic() < deadline, "shadow margin covered the desktop"
        time.sleep(POLL_INTERVAL)


def manager_window(manager, setup, root):
    base, = struct.unpack_from("<I", setup, RESOURCE_BASE_OFFSET)
    window = base | 1
    manager.sendall(struct.pack("<BBHIIhhHHHHII", CREATE_WINDOW, COPY_FROM_PARENT,
                                8, window, root, 0, 0, 1, 1, 0, INPUT_ONLY,
                                COPY_FROM_PARENT, 0))
    return window


def set_compositor(manager, window, selection):
    manager.sendall(struct.pack("<BBHIII", SET_SELECTION_OWNER, 0, 4,
                                window, selection, CURRENT_TIME))


def check_shadows(path, display, x11, root, scale=1, transform=0, composited=False):
    before = query_tree(x11, root)
    client = XdgClient(path)
    manager, setup = connect_x11(display, return_setup=True)
    try:
        window, = query_tree(x11, root) - before
        selection = intern_atom(manager, "_NET_WM_CM_S0")
        owner = manager_window(manager, setup, root)
        if composited:
            set_compositor(manager, owner, selection)
        x, y, width, height = GEOMETRY
        client.send(client.xdg, XDG_GEOMETRY, struct.pack("=iiii", *GEOMETRY))
        client.commit()
        client.configure()
        rows = tuple(tuple(WINDOW_COLOR if x <= column < x + width and y <= row < y + height
                           else SHADOW if row and column else TRANSPARENT
                           for column in range(WIDTH)) for row in range(HEIGHT))
        client.send(client.surface, SURFACE_SCALE, struct.pack("=i", scale))
        client.send(client.surface, SURFACE_TRANSFORM, struct.pack("=i", transform))
        fd, buffer = attach_rows(client, transformed(rows, transform, scale))
        try:
            callback = client.frame()
            client.commit()
            client.frame_done(callback)
            client.send(buffer, 0)
        finally:
            os.close(fd)
        if composited:
            wait_shape(x11, window, FULL_BOUNDS)
            set_compositor(manager, 0, selection)
        wait_shape(x11, window, GEOMETRY_BOUNDS)
        expected = tuple(WINDOW_COLOR & RGB_MASK if x <= column < x + width and y <= row < y + height
                         else DESKTOP_COLOR for row in range(HEIGHT) for column in range(WIDTH))
        wait_desktop(x11, root, expected)
        assert property32(x11, window, "_GTK_FRAME_EXTENTS", XA_CARDINAL) == (
            x, WIDTH - x - width, y, HEIGHT - y - height)
        hints = property32(x11, window, "WM_NORMAL_HINTS", XA_WM_SIZE_HINTS)
        assert hints[0] == HINT_MIN_SIZE | HINT_RESIZE_INC | HINT_BASE_SIZE
        assert hints[HINT_BASE] == (WIDTH - width, HEIGHT - height)
        assert hints[HINT_INCREMENTS] == (1, 1)
        # Size readouts express native dimensions relative to the shadow base.
        native_size = (WIDTH, HEIGHT)
        assert tuple((size - base) // increment for size, base, increment in
                     zip(native_size, hints[HINT_BASE], hints[HINT_INCREMENTS])) == (width, height)

        set_compositor(manager, owner, selection)
        wait_shape(x11, window, FULL_BOUNDS)
        # The compositor receives the original alpha and translucent shadow pixels.
        expected_pixels = tuple(pixel for row in rows for pixel in row)
        deadline = time.monotonic() + STARTUP_TIMEOUT
        while image(x11, window, WIDTH, HEIGHT) != expected_pixels:
            assert time.monotonic() < deadline, "alpha pixels changed while restoring shadows"
            time.sleep(POLL_INTERVAL)

        manager.sendall(struct.pack("<BBHI", DESTROY_WINDOW, 0, 2, owner))
        wait_shape(x11, window, GEOMETRY_BOUNDS)
        wait_desktop(x11, root, expected)
        owner = manager_window(manager, setup, root)
        set_compositor(manager, owner, selection)
        wait_shape(x11, window, FULL_BOUNDS)
        set_compositor(manager, 0, selection)
        wait_shape(x11, window, GEOMETRY_BOUNDS)
        set_compositor(manager, owner, selection)
        wait_shape(x11, window, FULL_BOUNDS)
        manager.close()
        manager = None
        wait_shape(x11, window, GEOMETRY_BOUNDS)
        wait_desktop(x11, root, expected)

        # Geometry remains double-buffered and can change without a new buffer.
        client.send(client.xdg, XDG_GEOMETRY, struct.pack("=iiii", 1, 1, WIDTH - 2, HEIGHT - 2))
        client.sync()
        wait_shape(x11, window, GEOMETRY_BOUNDS)
        client.commit()
        client.sync()
        wait_shape(x11, window, [(1, 1, WIDTH - 2, HEIGHT - 2)])
        for geometry, bounds in (((-1, -1, WIDTH + 2, HEIGHT + 2), FULL_BOUNDS),
                                 ((WIDTH + 1, HEIGHT + 1, 1, 1), [])):
            client.send(client.xdg, XDG_GEOMETRY, struct.pack("=iiii", *geometry))
            client.commit()
            client.sync()
            wait_shape(x11, window, bounds)
            if bounds == FULL_BOUNDS:
                hints = property32(x11, window, "WM_NORMAL_HINTS", XA_WM_SIZE_HINTS)
                assert not hints[0] & (HINT_BASE_SIZE | HINT_RESIZE_INC)
                assert hints[HINT_BASE] == (0, 0)
        client.send(client.xdg, XDG_GEOMETRY, struct.pack("=iiii", *GEOMETRY))
        client.commit()
        client.sync()
        wait_shape(x11, window, GEOMETRY_BOUNDS)
        # Unmapping and remapping clear and restore the fallback.
        client.attach(0)
        client.commit()
        client.sync()
        wait_shape(x11, window, FULL_BOUNDS)
        client.send(client.xdg, XDG_GEOMETRY, struct.pack("=iiii", *GEOMETRY))
        client.commit()
        client.configure()
        fd, buffer = attach_rows(client, transformed(rows, transform, scale))
        try:
            callback = client.frame()
            client.commit()
            client.frame_done(callback)
        finally:
            os.close(fd)
        wait_shape(x11, window, GEOMETRY_BOUNDS)
        client.send(client.top, TOP_DESTROY)
        client.send(client.xdg, XDG_DESTROY)
        client.sync()
        wait_shape(x11, window, FULL_BOUNDS)
    finally:
        if manager:
            manager.close()
        client.close()


def main():
    host, module = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="meatball-shadows-") as directory:
        path = Path(directory) / "wayland"
        with server(host, module, {"MEATBALL_TEST_SOCKET": str(path)}) as (process, display):
            wait_socket(path, process)
            x11, setup = connect_x11(display, return_setup=True)
            try:
                root = root_window(setup)
                x11.sendall(struct.pack("<BBHIII", CHANGE_ATTRIBUTES, 0, 4,
                                        root, CW_BACKGROUND_PIXEL, DESKTOP_COLOR))
                x11.sendall(struct.pack("<BBHIhhHH", CLEAR_AREA, 0, 4, root, 0, 0, 0, 0))
                check_shadows(path, display, x11, root)
                check_shadows(path, display, x11, root, scale=2, transform=1, composited=True)
                client = XdgClient(path)
                client.send(client.xdg, XDG_GEOMETRY, struct.pack("=iiii", 1, 1, 1, 1))
                client.commit()
                client.configure()
                fd, buffer = attach_rows(client, ((SHADOW, SHADOW), (SHADOW, WINDOW_COLOR)))
                try:
                    callback = client.frame()
                    client.commit()
                    client.frame_done(callback)
                finally:
                    os.close(fd)
                process.send_signal(signal.SIGHUP)
                assert x11.recv(1) == b""
                client.disconnected()
                client.close()
            finally:
                x11.close()
            with connect_x11(display) as x11:
                client = XdgClient(path)
                client.commit()
                client.configure()
                client.close()


if __name__ == "__main__":
    main()
