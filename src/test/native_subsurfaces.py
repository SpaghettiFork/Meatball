"""Exercise native subsurface content updates, parent state, and stacking."""

import os
from pathlib import Path
import signal
import struct
import sys
import tempfile

from host_support import connect_x11, receive_exact, server
from native_lifecycle import wait_socket
from native_surfaces import create_surface, root_window, query_tree, shape_rectangles
from native_transforms import attach_rows, wait_image, transformed, SURFACE_SCALE, SURFACE_TRANSFORM
from native_xdg import XdgClient, map_state, XDG_GEOMETRY, XDG_GET_SURFACE
from native_clients import KILL_CLIENT, geometry
from native_viewport import create_viewport, source, destination
from wayland_support import Wayland, WaylandProtocolError

DESTROY = 0
GET_SUBSURFACE = 1
POSITION = 1
ABOVE = 2
BELOW = 3
SYNC = 4
DESYNC = 5
ATTACH = 1
FRAME = 3
INPUT = 5
COMMIT = 6
CONFIGURE_WINDOW = 12
GET_GEOMETRY = 14
CLEAR_AREA = 61
CW_POSITION = (1 << 0) | (1 << 1)
UNMAPPED = 0
RED = 0xffc00000
GREEN = 0xff00c000
BLUE = 0xff0000c0
HALF_GREEN = 0x80008000
RED_GREEN = 0xff608000
TRANSPARENT = 0
SIZE = 8
WINDOW_POSITION = 24
TREE_DEPTH = 40
X11_REPLY_SIZE = 32
INT32_MAX = (1 << 31) - 1
BAD_SURFACE = 0
BAD_PARENT = 1
XDG_INVALID_SURFACE_STATE = 4
SURFACE_INVALID_SIZE = 2


def solid(color, width=SIZE, height=SIZE):
    return tuple((color,) * width for _ in range(height))


def rectangle(rows, color, x, y, width, height):
    return tuple(tuple(color if x <= column < x + width and y <= row < y + height else value
                       for column, value in enumerate(values)) for row, values in enumerate(rows))


def commit(client, surface):
    client.send(surface, COMMIT)


def publish(client, surface, rows):
    fd, buffer = attach_rows(client, rows, surface)
    try:
        commit(client, surface)
        events = client.roundtrip_events()
        assert (buffer, 0, b"") in events
        os.pwrite(fd, bytes(os.fstat(fd).st_size), 0)
        client.send(buffer, DESTROY)
        return events
    finally:
        os.close(fd)


def child(client, parent, x=0, y=0, desync=False):
    surface = create_surface(client, client.compositor)
    role = client.allocate_id()
    client.send(client.subcompositor, GET_SUBSURFACE, struct.pack("=III", role, surface, parent))
    client.send(role, POSITION, struct.pack("=ii", x, y))
    if desync:
        client.send(role, DESYNC)
    client.sync()
    return surface, role


def main_window(client, x11, root, rows=None):
    window, = query_tree(x11, root)
    client.commit()
    client.configure()
    publish(client, client.surface, solid(RED) if rows is None else rows)
    x11.sendall(struct.pack("<BBHIHHii", CONFIGURE_WINDOW, 0, 5,
                            window, CW_POSITION, 0, WINDOW_POSITION, WINDOW_POSITION))
    wait_image(x11, window, solid(RED) if rows is None else rows)
    return window


def new_client(path):
    client = XdgClient(path)
    client.subcompositor = client.bind("wl_subcompositor")
    return client


def check_sync(path, x11, root):
    client = new_client(path)
    try:
        window = main_window(client, x11, root)
        surface, role = child(client, client.surface, 2, 2)
        callback = client.allocate_id()
        client.send(surface, FRAME, struct.pack("=I", callback))
        events = publish(client, surface, solid(BLUE, 4, 4))
        assert not any(object_id == callback for object_id, _, _ in events)
        wait_image(x11, window, solid(RED))
        client.commit()
        client.frame_done(callback)
        wait_image(x11, window, rectangle(solid(RED), BLUE, 2, 2, 4, 4))
        callbacks = []
        for color in (GREEN, BLUE, HALF_GREEN):
            callbacks.append(client.allocate_id())
            client.send(surface, FRAME, struct.pack("=I", callbacks[-1]))
            events = publish(client, surface, solid(color, 4, 4))
            assert not any(object_id in callbacks for object_id, _, _ in events)
        wait_image(x11, window, rectangle(solid(RED), BLUE, 2, 2, 4, 4))
        client.commit()
        for callback in callbacks:
            client.frame_done(callback)
        wait_image(x11, window, rectangle(solid(RED), RED_GREEN, 2, 2, 4, 4))
        # Pending position and stack changes apply on the parent's commit.
        client.send(role, DESYNC)
        client.send(role, POSITION, struct.pack("=ii", 1, 1))
        publish(client, surface, solid(BLUE, 4, 4))
        wait_image(x11, window, rectangle(solid(RED), BLUE, 2, 2, 4, 4))
        client.commit()
        client.sync()
        wait_image(x11, window, rectangle(solid(RED), BLUE, 1, 1, 4, 4))
        x11.sendall(struct.pack("<BBHIhhHH", CLEAR_AREA, 1, 4, window, 0, 0, SIZE, SIZE))
        wait_image(x11, window, rectangle(solid(RED), BLUE, 1, 1, 4, 4))
        client.send(role, BELOW, struct.pack("=I", client.surface))
        client.sync()
        wait_image(x11, window, rectangle(solid(RED), BLUE, 1, 1, 4, 4))
        client.commit()
        client.sync()
        wait_image(x11, window, solid(RED))
        publish(client, client.surface, rectangle(solid(RED), TRANSPARENT, 2, 2, 2, 2))
        wait_image(x11, window, rectangle(solid(RED), BLUE, 2, 2, 2, 2))
        client.send(role, ABOVE, struct.pack("=I", client.surface))
        client.commit()
        client.sync()
        wait_image(x11, window, rectangle(solid(RED), BLUE, 1, 1, 4, 4))
        client.send(role, DESTROY)
        client.sync()
        wait_image(x11, window, rectangle(solid(RED), TRANSPARENT, 2, 2, 2, 2))
    finally:
        client.close()


def check_nested(path, x11, root):
    client = new_client(path)
    try:
        window = main_window(client, x11, root)
        first, first_role = child(client, client.surface, 1, 1)
        second, second_role = child(client, first, 1, 1, desync=True)
        publish(client, second, solid(BLUE, 2, 2))
        publish(client, first, solid(GREEN, 5, 5))
        client.commit()
        client.sync()
        expected = rectangle(rectangle(solid(RED), GREEN, 1, 1, 5, 5), BLUE, 2, 2, 2, 2)
        wait_image(x11, window, expected)
        # The parent captures one child update. A later update waits for another parent commit.
        publish(client, second, solid(RED, 2, 2))
        commit(client, first)
        publish(client, second, solid(BLUE, 2, 2))
        client.commit()
        client.sync()
        wait_image(x11, window, rectangle(rectangle(solid(RED), GREEN, 1, 1, 5, 5), RED, 2, 2, 2, 2))
        commit(client, first)
        client.commit()
        client.sync()
        wait_image(x11, window, expected)
        # Desynchronizing an ancestor applies its cached descendants immediately.
        publish(client, second, solid(RED, 2, 2))
        commit(client, first)
        client.send(first_role, DESYNC)
        client.sync()
        wait_image(x11, window, rectangle(rectangle(solid(RED), GREEN, 1, 1, 5, 5), RED, 2, 2, 2, 2))
        publish(client, second, solid(BLUE, 2, 2))
        wait_image(x11, window, expected)
        # A pending new child remains hidden until the desynchronized parent commits.
        third, third_role = child(client, first, 0, 0, desync=True)
        publish(client, third, solid(BLUE, 1, 1))
        wait_image(x11, window, expected)
        commit(client, first)
        client.sync()
        wait_image(x11, window, rectangle(expected, BLUE, 1, 1, 1, 1))
    finally:
        client.close()


def check_stacking_and_bounds(path, x11, root):
    client = new_client(path)
    try:
        window = main_window(client, x11, root)
        first, first_role = child(client, client.surface, 1, 1)
        second, second_role = child(client, client.surface, 3, 3)
        publish(client, first, solid(BLUE, 4, 4))
        publish(client, second, solid(GREEN, 4, 4))
        client.commit()
        client.sync()
        expected = rectangle(rectangle(solid(RED), BLUE, 1, 1, 4, 4), GREEN, 3, 3, 4, 4)
        wait_image(x11, window, expected)
        assert all(map_state(x11, child_window) == UNMAPPED
                   for child_window in query_tree(x11, root) - {window})
        client.send(first_role, ABOVE, struct.pack("=I", second))
        client.sync()
        wait_image(x11, window, expected)
        client.commit()
        client.sync()
        wait_image(x11, window, rectangle(rectangle(solid(RED), GREEN, 3, 3, 4, 4), BLUE, 1, 1, 4, 4))
        client.send(first_role, BELOW, struct.pack("=I", second))
        client.commit()
        client.sync()
        wait_image(x11, window, expected)
        client.send(second_role, DESTROY)
        client.send(second, DESTROY)
        client.send(first_role, POSITION, struct.pack("=ii", -2, -1))
        publish(client, first, solid(BLUE, 2, 2))
        client.send(client.xdg, XDG_GEOMETRY, struct.pack("=iiii", 0, 0, SIZE, SIZE))
        client.commit()
        client.sync()
        expected = rectangle(rectangle(solid(TRANSPARENT, SIZE + 2, SIZE + 1), RED,
                                       2, 1, SIZE, SIZE), BLUE, 0, 0, 2, 2)
        wait_image(x11, window, expected)
        x11.sendall(struct.pack("<BBHI", GET_GEOMETRY, 0, 2, window))
        reply = receive_exact(x11, X11_REPLY_SIZE)
        assert struct.unpack_from("<hh", reply, 12) == (WINDOW_POSITION - 2, WINDOW_POSITION - 1)
        # An empty input region on the child leaves only the translated parent region.
        region = client.allocate_id()
        client.send(client.compositor, 1, struct.pack("=I", region))
        client.send(first, INPUT, struct.pack("=I", region))
        client.send(region, DESTROY)
        commit(client, first)
        client.commit()
        client.sync()
        assert shape_rectangles(x11, window) == [(2, 1, SIZE, SIZE)]
        client.send(first, ATTACH, struct.pack("=Iii", 0, 0, 0))
        commit(client, first)
        client.commit()
        client.sync()
        wait_image(x11, window, solid(RED))
        x11.sendall(struct.pack("<BBHI", GET_GEOMETRY, 0, 2, window))
        reply = receive_exact(x11, X11_REPLY_SIZE)
        assert struct.unpack_from("<hh", reply, 12) == (WINDOW_POSITION, WINDOW_POSITION)
    finally:
        client.close()


def check_cached_parent_state(path, x11, root):
    client = new_client(path)
    try:
        window = main_window(client, x11, root)
        first, first_role = child(client, client.surface, 1, 1)
        second, second_role = child(client, first, 1, 1)
        publish(client, second, solid(BLUE, 2, 2))
        publish(client, first, solid(GREEN, 5, 5))
        client.send(second_role, POSITION, struct.pack("=ii", 2, 2))
        third, third_role = child(client, first, 0, 0)
        publish(client, third, solid(BLUE, 1, 1))
        client.commit()
        client.sync()
        expected = rectangle(rectangle(solid(RED), GREEN, 1, 1, 5, 5), BLUE, 2, 2, 2, 2)
        wait_image(x11, window, expected)
        commit(client, first)
        client.commit()
        client.sync()
        expected = rectangle(rectangle(rectangle(solid(RED), GREEN, 1, 1, 5, 5),
                                       BLUE, 3, 3, 2, 2), BLUE, 1, 1, 1, 1)
        wait_image(x11, window, expected)
        viewport = create_viewport(client, client.bind("wp_viewporter"), second)
        source(client, viewport, (0, 0, 1, 1))
        destination(client, viewport, (1, 1))
        commit(client, second)
        commit(client, first)
        client.send(viewport, DESTROY)
        commit(client, second)
        client.commit()
        client.sync()
        cropped = rectangle(rectangle(rectangle(solid(RED), GREEN, 1, 1, 5, 5),
                                      BLUE, 3, 3, 1, 1), BLUE, 1, 1, 1, 1)
        wait_image(x11, window, cropped)
        commit(client, first)
        client.commit()
        client.sync()
        wait_image(x11, window, expected)
    finally:
        client.close()


def check_destruction(path, x11, root):
    client = new_client(path)
    try:
        window = main_window(client, x11, root)
        first, first_role = child(client, client.surface, 1, 1)
        second, second_role = child(client, first, 1, 1)
        publish(client, second, solid(BLUE, 2, 2))
        publish(client, first, solid(GREEN, 5, 5))
        client.commit()
        client.sync()
        expected = rectangle(rectangle(solid(RED), GREEN, 1, 1, 5, 5), BLUE, 2, 2, 2, 2)
        wait_image(x11, window, expected)
        # Destroying a role hides its entire tree. The surface can regain the same role.
        client.send(first_role, DESTROY)
        client.sync()
        wait_image(x11, window, solid(RED))
        first_role = client.allocate_id()
        client.send(client.subcompositor, GET_SUBSURFACE,
                    struct.pack("=III", first_role, first, client.surface))
        client.send(first_role, POSITION, struct.pack("=ii", 1, 1))
        client.commit()
        client.sync()
        wait_image(x11, window, expected)
        # Removing a child also removes dependencies held by its cached parent.
        publish(client, second, solid(RED, 2, 2))
        commit(client, first)
        client.send(second, DESTROY)
        client.commit()
        client.sync()
        wait_image(x11, window, rectangle(solid(RED), GREEN, 1, 1, 5, 5))
        client.send(second_role, DESYNC)
        client.send(second_role, DESTROY)
        second, second_role = child(client, first, 0, 0)
        publish(client, second, solid(BLUE, 2, 2))
        commit(client, first)
        client.send(first, DESTROY)
        client.commit()
        client.sync()
        wait_image(x11, window, solid(RED))
        client.send(first_role, DESTROY)
        client.send(second_role, DESYNC)
        client.send(second_role, DESTROY)
        client.send(second, DESTROY)
        client.sync()
        geometry(x11, window, True)
    finally:
        client.close()


def check_errors(path):
    observer = Wayland(path)
    try:
        for failure in ("role", "duplicate", "cycle", "self", "stack-self", "stack-unrelated", "permanent-role"):
            client = new_client(path)
            try:
                first, first_role = child(client, client.surface)
                second = create_surface(client, client.compositor)
                target = client.subcompositor
                code = BAD_SURFACE
                if failure == "role":
                    client.send(target, GET_SUBSURFACE, struct.pack("=III", client.allocate_id(), client.surface, first))
                elif failure == "duplicate":
                    client.send(target, GET_SUBSURFACE, struct.pack("=III", client.allocate_id(), first, client.surface))
                elif failure in ("cycle", "self"):
                    code = BAD_PARENT
                    if failure == "cycle":
                        other = create_surface(client, client.compositor)
                        client.send(target, GET_SUBSURFACE, struct.pack("=III", client.allocate_id(), other, second))
                        client.send(target, GET_SUBSURFACE, struct.pack("=III", client.allocate_id(), second, other))
                    else:
                        client.send(target, GET_SUBSURFACE, struct.pack("=III", client.allocate_id(), second, second))
                elif failure.startswith("stack-"):
                    client.send(first_role, ABOVE, struct.pack("=I", first if failure == "stack-self" else second))
                    target = first_role
                else:
                    client.send(first_role, DESTROY)
                    client.send(client.base, XDG_GET_SURFACE, struct.pack("=II", client.allocate_id(), first))
                    target = client.base
                    code = XDG_INVALID_SURFACE_STATE
                try:
                    client.sync()
                    raise AssertionError(f"invalid subsurface request accepted: {failure}")
                except WaylandProtocolError as error:
                    assert (error.object_id, error.code) == (target, code)
            finally:
                client.close()
            observer.sync()
    finally:
        observer.close()


def check_deep_tree(path, x11, root):
    client = new_client(path)
    try:
        window = main_window(client, x11, root)
        surfaces = [client.surface]
        for _ in range(TREE_DEPTH):
            surface, role = child(client, surfaces[-1])
            surfaces.append(surface)
        for surface in reversed(surfaces[1:]):
            publish(client, surface, solid(BLUE, 1, 1))
        client.commit()
        client.sync()
        wait_image(x11, window, rectangle(solid(RED), BLUE, 0, 0, 1, 1))
        x11.sendall(struct.pack("<BBHI", KILL_CLIENT, 0, 2, window))
        client.disconnected()
    finally:
        client.close()


def check_child_transform(path, x11, root):
    client = new_client(path)
    try:
        window = main_window(client, x11, root)
        surface, role = child(client, client.surface, 1, 1)
        rows = ((BLUE, GREEN), (GREEN, BLUE), (RED, HALF_GREEN))
        client.send(surface, SURFACE_SCALE, struct.pack("=i", 2))
        client.send(surface, SURFACE_TRANSFORM, struct.pack("=i", 1))
        publish(client, surface, transformed(rows, 1, 2))
        client.commit()
        client.sync()
        expected = solid(RED)
        for y, values in enumerate(rows, 1):
            for x, color in enumerate(values, 1):
                expected = rectangle(expected, RED_GREEN if color == HALF_GREEN else color, x, y, 1, 1)
        wait_image(x11, window, expected)
        viewport = create_viewport(client, client.bind("wp_viewporter"), surface)
        source(client, viewport, (1, 0, 1, 2))
        destination(client, viewport, (2, 4))
        commit(client, surface)
        client.commit()
        client.sync()
        expected = rectangle(rectangle(solid(RED), GREEN, 1, 1, 2, 2), BLUE, 1, 3, 2, 2)
        wait_image(x11, window, expected)
    finally:
        client.close()


def check_canvas_limits(path, x11, root):
    client = new_client(path)
    try:
        main_window(client, x11, root)
        first, first_role = child(client, client.surface, INT32_MAX, INT32_MAX)
        second, second_role = child(client, first, INT32_MAX, INT32_MAX)
        publish(client, second, solid(BLUE, 1, 1))
        publish(client, first, solid(GREEN, 1, 1))
        client.commit()
        try:
            client.sync()
            raise AssertionError("oversized subsurface tree accepted")
        except WaylandProtocolError as error:
            assert (error.object_id, error.code) == (client.surface, SURFACE_INVALID_SIZE)
    finally:
        client.close()


def main():
    host, module = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="meatball-subsurfaces-") as directory:
        path = Path(directory) / "wayland"
        with server(host, module, {"MEATBALL_TEST_SOCKET": str(path)}) as (process, display):
            wait_socket(path, process)
            x11, setup = connect_x11(display, return_setup=True)
            try:
                root = root_window(setup)
                check_sync(path, x11, root)
                # A Wayland round trip drains the previous connection's teardown.
                observer = Wayland(path)
                observer.sync()
                check_nested(path, x11, root)
                observer.sync()
                check_stacking_and_bounds(path, x11, root)
                observer.sync()
                check_cached_parent_state(path, x11, root)
                observer.sync()
                check_child_transform(path, x11, root)
                observer.sync()
                check_destruction(path, x11, root)
                observer.sync()
                check_errors(path)
                observer.sync()
                check_deep_tree(path, x11, root)
                observer.sync()
                check_canvas_limits(path, x11, root)
                observer.sync()
                observer.close()
                client = new_client(path)
                main_window(client, x11, root)
                first, first_role = child(client, client.surface)
                second, second_role = child(client, first)
                publish(client, second, solid(BLUE, 2, 2))
                publish(client, first, solid(GREEN, 4, 4))
                process.send_signal(signal.SIGHUP)
                assert x11.recv(1) == b""
                client.disconnected()
                client.close()
                assert process.poll() is None
            finally:
                x11.close()
            x11, setup = connect_x11(display, return_setup=True)
            try:
                wait_socket(path, process)
                check_sync(path, x11, root_window(setup))
            finally:
                x11.close()


if __name__ == "__main__":
    main()
