"""Check popup placement and lifetime on native X11 windows."""

from pathlib import Path
import struct
import sys
import tempfile

from host_support import connect_x11, receive_exact, server
from native_input import XInput
from native_lifecycle import wait_socket
from native_subsurfaces import (CONFIGURE_WINDOW, CW_POSITION, GET_GEOMETRY,
                                X11_REPLY_SIZE, publish, solid)
from native_surfaces import create_surface, input_output_windows, query_tree, root_window
from native_transforms import image
from native_xdg import (CONFIGURE_EVENT, UNMAPPED, VIEWABLE, XDG_ACK_CONFIGURE,
                        XDG_GEOMETRY, XDG_GET_SURFACE, XdgClient, map_state)

CREATE_POSITIONER = 1
POSITIONER_DESTROY = 0
SET_SIZE = 1
SET_ANCHOR_RECT = 2
SET_ANCHOR = 3
SET_GRAVITY = 4
SET_ADJUSTMENT = 5
SET_OFFSET = 6
GET_POPUP = 2
POPUP_DESTROY = 0
POPUP_DONE = 1
BOTTOM_RIGHT = 8
FLIP_X = 4
SLIDE_Y = 2
RESIZE_X = 16
COMMIT = 6
DESTROY = 0
WIDTH = 64
HEIGHT = 48
SCREEN_WIDTH = 640
SCREEN_HEIGHT = 480
PARENT_X = SCREEN_WIDTH - 50
PARENT_Y = SCREEN_HEIGHT - 62
POPUP_COLOR = 0xff4090a0
PARENT_COLOR = 0xff304050


def move(actor, window, x, y):
    actor.connection.sendall(struct.pack("<BBHIHHii", CONFIGURE_WINDOW, 0, 5,
                                         window, CW_POSITION, 0, x, y))
    actor.sync()


def geometry(x11, window):
    x11.sendall(struct.pack("<BBHI", GET_GEOMETRY, 0, 2, window))
    reply = receive_exact(x11, X11_REPLY_SIZE)
    assert reply[0] == 1, reply
    return struct.unpack_from("<hhHH", reply, 12)


def new_popup(client, parent, size=(20, 24), anchor=(40, 35, 5, 5),
              adjustment=FLIP_X | SLIDE_Y, offset=(0, 0)):
    surface = create_surface(client, client.compositor)
    positioner, shell, popup = (client.allocate_id() for _ in range(3))
    client.send(client.base, CREATE_POSITIONER, struct.pack("=I", positioner))
    client.send(positioner, SET_SIZE, struct.pack("=ii", *size))
    client.send(positioner, SET_ANCHOR_RECT, struct.pack("=iiii", *anchor))
    client.send(positioner, SET_ANCHOR, struct.pack("=I", BOTTOM_RIGHT))
    client.send(positioner, SET_GRAVITY, struct.pack("=I", BOTTOM_RIGHT))
    client.send(positioner, SET_ADJUSTMENT, struct.pack("=I", adjustment))
    client.send(positioner, SET_OFFSET, struct.pack("=ii", *offset))
    client.send(client.base, XDG_GET_SURFACE, struct.pack("=II", shell, surface))
    client.send(shell, GET_POPUP, struct.pack("=III", popup, parent, positioner))
    client.send(positioner, POSITIONER_DESTROY)
    client.sync()
    return surface, shell, popup


def configure_popup(client, surface, shell, popup):
    client.send(surface, COMMIT)
    rect = None
    while True:
        resource, opcode, data = client.receive()
        if resource == popup and opcode == CONFIGURE_EVENT:
            rect = struct.unpack("=iiii", data)
        if resource == shell and opcode == CONFIGURE_EVENT:
            assert rect is not None
            client.send(shell, XDG_ACK_CONFIGURE, data)
            return rect


def destroy_popup(client, surface, shell, popup):
    client.send(popup, POPUP_DESTROY)
    client.send(shell, DESTROY)
    client.send(surface, DESTROY)
    client.sync()


def parent_window(client, actor, root):
    window, = input_output_windows(actor.connection, root)
    # The geometry begins inside the root buffer.
    client.send(client.xdg, XDG_GEOMETRY, struct.pack("=iiii", 2, 3, WIDTH, HEIGHT))
    client.commit()
    client.configure()
    publish(client, client.surface, solid(PARENT_COLOR, WIDTH + 4, HEIGHT + 6))
    move(actor, window, PARENT_X, PARENT_Y)
    client.sync()
    return window


def check_popup(path, actor, root):
    client = XdgClient(path)
    x11 = actor.connection
    try:
        window = parent_window(client, actor, root)
        before = query_tree(x11, root)
        surface, shell, popup = new_popup(client, client.xdg)
        rect = configure_popup(client, surface, shell, popup)
        assert rect == (20, 38, 20, 24), (rect, geometry(x11, window))
        native, = query_tree(x11, root) - before
        assert map_state(x11, native) == UNMAPPED
        publish(client, surface, solid(POPUP_COLOR, 20, 24))
        assert geometry(x11, native) == (PARENT_X + 20, PARENT_Y + 38, 20, 24)
        assert map_state(x11, native) == VIEWABLE
        assert image(x11, native, 20, 24) == (POPUP_COLOR,) * (20 * 24)

        before = query_tree(x11, root)
        sub = new_popup(client, shell, size=(28, 9), anchor=(10, 9, 5, 4),
                        adjustment=RESIZE_X, offset=(1, -1))
        assert configure_popup(client, *sub) == (16, 12, 14, 9)
        publish(client, sub[0], solid(POPUP_COLOR, 14, 9))
        nested, = query_tree(x11, root) - before
        assert geometry(x11, nested) == (PARENT_X + 36, PARENT_Y + 50, 14, 9)
        move(actor, window, PARENT_X - 10, PARENT_Y - 12)
        client.sync()
        assert geometry(x11, native) == (PARENT_X + 10, PARENT_Y + 26, 20, 24)
        assert geometry(x11, nested) == (PARENT_X + 26, PARENT_Y + 38, 14, 9)

        client.attach(0)
        client.commit()
        events = client.roundtrip_events()
        assert [resource for resource, opcode, _ in events
                if opcode == POPUP_DONE and resource in (popup, sub[2])] == [sub[2], popup]
        assert map_state(x11, native) == UNMAPPED
        assert map_state(x11, nested) == UNMAPPED
        destroy_popup(client, *sub)
        destroy_popup(client, surface, shell, popup)
    finally:
        client.close()


def main():
    host, module = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="meatball-popup-") as directory:
        path = Path(directory) / "wayland"
        with server(host, module, {"MEATBALL_TEST_SOCKET": str(path)},
                    arguments=["-screen", "0", f"{SCREEN_WIDTH}x{SCREEN_HEIGHT}x24",
                               "-crtcs", f"1@{SCREEN_WIDTH}x{SCREEN_HEIGHT}",
                               "+extension", "XTEST"]) as (process, display):
            wait_socket(path, process)
            x11, setup = connect_x11(display, return_setup=True)
            try:
                root = root_window(setup)
                check_popup(path, XInput(x11, root), root)
                assert process.poll() is None
            finally:
                x11.close()


if __name__ == "__main__":
    main()
