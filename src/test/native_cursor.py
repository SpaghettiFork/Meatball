"""Read native cursor pixels after Wayland cursor commits and role changes."""

from pathlib import Path
import struct
import sys
import tempfile

from host_support import connect_x11, receive_exact, server
from native_input import (InputClient, XInput, extension, map_window, MOTION,
                          WINDOW_ORIGIN, POINTER_ENTER, X11_REPLY_SIZE)
from native_lifecycle import wait_socket
from native_subsurfaces import child, publish, solid, BLUE
from native_surfaces import create_surface, root_window
from wayland_support import WaylandProtocolError

SET_CURSOR = 0
CURSOR_ROLE_ERROR = 0
SURFACE_FRAME = 3
SURFACE_COMMIT = 6
SURFACE_SCALE = 8
XFIXES_QUERY_VERSION = 0
XFIXES_GET_CURSOR_IMAGE = 4
PIXELS = ((0xffff0000, 0x80008000), (0, 0xff0000ff))


def cursor_image(x11, opcode):
    x11.sendall(struct.pack("<BBH", opcode, XFIXES_GET_CURSOR_IMAGE, 1))
    reply = receive_exact(x11, X11_REPLY_SIZE)
    assert reply[0] == 1, reply.hex()
    width, height, hot_x, hot_y = struct.unpack_from("<HHHH", reply, 12)
    pixels = receive_exact(x11, width * height * 4)
    return (width, height, hot_x, hot_y), struct.unpack(f"<{width * height}I", pixels)


def check_cursor(path, x11, root):
    client = InputClient(path)
    actor = XInput(x11, root)
    fixes = extension(x11, "XFIXES")
    x11.sendall(struct.pack("<BBHII", fixes, XFIXES_QUERY_VERSION, 3, 5, 0))
    assert receive_exact(x11, X11_REPLY_SIZE)[0] == 1
    try:
        map_window(client, x11, root)
        actor.fake(MOTION, x=0, y=0)
        actor.sync()
        client.sync()
        actor.fake(MOTION, x=WINDOW_ORIGIN + 5, y=WINDOW_ORIGIN + 7)
        data, _ = client.until(client.pointer, POINTER_ENTER)
        serial, = struct.unpack_from("=I", data)
        surface = create_surface(client, client.compositor)
        client.send(client.pointer, SET_CURSOR, struct.pack("=IIii", serial, surface, 1, 1))
        callback = client.allocate_id()
        client.send(surface, SURFACE_FRAME, struct.pack("=I", callback))
        events = publish(client, surface, PIXELS)
        if not any(object_id == callback for object_id, _, _ in events):
            client.until(callback, 0)
        assert cursor_image(x11, fixes) == ((2, 2, 1, 1), sum(PIXELS, ()))
        # A stale serial cannot change the focused client's cursor.
        client.send(client.pointer, SET_CURSOR, struct.pack("=IIii", serial - 1, 0, 0, 0))
        client.sync()
        assert cursor_image(x11, fixes) == ((2, 2, 1, 1), sum(PIXELS, ()))
        # Negative hotspots need transparent padding in the native cursor.
        client.send(client.pointer, SET_CURSOR, struct.pack("=IIii", serial, surface, -1, 0))
        client.sync()
        assert cursor_image(x11, fixes) == ((3, 2, 0, 0),
                                           (0,) + PIXELS[0] + (0,) + PIXELS[1])
        client.send(client.pointer, SET_CURSOR, struct.pack("=IIii", serial, surface, 0, 0))
        client.send(surface, SURFACE_SCALE, struct.pack("=i", 2))
        publish(client, surface, solid(BLUE, 4, 2))
        assert cursor_image(x11, fixes) == ((2, 1, 0, 0), (BLUE, BLUE))
        nested, role = child(client, surface, 2, 0)
        publish(client, nested, solid(BLUE, 1, 1))
        client.send(surface, SURFACE_COMMIT)
        client.sync()
        assert cursor_image(x11, fixes) == ((3, 1, 0, 0), (BLUE,) * 3)
        client.send(client.pointer, SET_CURSOR, struct.pack("=IIii", serial, 0, 0, 0))
        client.sync()
        assert cursor_image(x11, fixes) == ((1, 1, 0, 0), (0,))
        client.send(surface, 0)
        client.sync()
        client.send(client.pointer, SET_CURSOR, struct.pack("=IIii", serial, client.surface, 0, 0))
        try:
            client.sync()
            raise AssertionError("toplevel accepted a cursor role")
        except WaylandProtocolError as error:
            assert error.object_id == client.pointer and error.code == CURSOR_ROLE_ERROR
    finally:
        client.close()


def main():
    host, module = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="meatball-cursor-") as directory:
        path = Path(directory) / "wayland"
        with server(host, module, {"MEATBALL_TEST_SOCKET": str(path)},
                    arguments=["+extension", "XTEST"]) as (process, display):
            wait_socket(path, process)
            x11, setup = connect_x11(display, return_setup=True)
            try:
                check_cursor(path, x11, root_window(setup))
                assert process.poll() is None
            finally:
                x11.close()


if __name__ == "__main__":
    main()
