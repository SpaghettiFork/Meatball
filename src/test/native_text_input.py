"""Exercise text-input-v3 and input-method-v2 through native keyboard focus."""

import os
from pathlib import Path
import struct
import sys
import tempfile

from host_support import connect_x11, server
from native_input import (InputClient, XInput, KEYBOARD_KEY, KEY_PRESS,
                          KEY_RELEASE, KEY_A, KEYCODE_OFFSET,
                          TEST_REPEAT_DELAY, map_window)
from native_lifecycle import wait_socket
from native_outputs import string
from native_subsurfaces import BLUE, publish, solid
from native_surfaces import create_surface, query_tree, root_window
from native_xdg import map_state, string_argument
from wayland_support import Wayland

TEXT_MANAGER = "zwp_text_input_manager_v3"
METHOD_MANAGER = "zwp_input_method_manager_v2"
TEXT_GET_INPUT = 1
TEXT_ENABLE, TEXT_DISABLE, TEXT_SURROUNDING, TEXT_CAUSE = 1, 2, 3, 4
TEXT_CONTENT, TEXT_RECTANGLE, TEXT_COMMIT = 5, 6, 7
TEXT_ENTER, TEXT_LEAVE, TEXT_PREEDIT, TEXT_STRING, TEXT_DELETE, TEXT_DONE = range(6)
METHOD_GET = 0
METHOD_STRING, METHOD_PREEDIT, METHOD_DELETE, METHOD_COMMIT = range(4)
METHOD_POPUP, METHOD_GRAB, METHOD_DESTROY = 4, 5, 6
METHOD_ACTIVATE, METHOD_DEACTIVATE, METHOD_SURROUNDING, METHOD_CAUSE = range(4)
METHOD_CONTENT, METHOD_DONE, METHOD_UNAVAILABLE = 4, 5, 6
GRAB_KEYMAP, GRAB_KEY, GRAB_MODIFIERS, GRAB_REPEAT = range(4)
GRAB_RELEASE = 0
POPUP_RECTANGLE = 0
VIEWABLE = 2
UNMAPPED = 0
CURSOR = (4, 5, 2, 8)


def send(client, resource, opcode, *args):
    client.send(resource, opcode, struct.pack("=" + "I" * len(args), *args))


def events_for(client, resource):
    return [(opcode, payload) for target, opcode, payload in client.roundtrip_events()
            if target == resource]


def check(path, display):
    x11, setup = connect_x11(display, return_setup=True)
    root = root_window(setup)
    actor = XInput(x11, root)
    app = InputClient(path)
    ime = Wayland(path)
    try:
        window = map_window(app, x11, root)
        text_manager = app.bind(TEXT_MANAGER)
        text = app.allocate_id()
        send(app, text_manager, TEXT_GET_INPUT, text, app.seat)
        app.sync()

        seat = ime.bind("wl_seat")
        method_manager = ime.bind(METHOD_MANAGER)
        method, duplicate = ime.allocate_id(), ime.allocate_id()
        send(ime, method_manager, METHOD_GET, seat, method)
        send(ime, method_manager, METHOD_GET, seat, duplicate)
        assert events_for(ime, duplicate) == [(METHOD_UNAVAILABLE, b"")]

        actor.focus(window)
        payload, _ = app.until(text, TEXT_ENTER)
        assert struct.unpack("=I", payload) == (app.surface,)

        send(app, text, TEXT_ENABLE)
        surrounding = "aéz"
        app.send(text, TEXT_SURROUNDING, string_argument(surrounding) + struct.pack("=ii", 3, 1))
        send(app, text, TEXT_CAUSE, 1)
        send(app, text, TEXT_CONTENT, 1, 0)
        send(app, text, TEXT_RECTANGLE, *CURSOR)
        app.sync()
        assert not events_for(ime, method)
        send(app, text, TEXT_COMMIT)
        app.sync()
        activation = events_for(ime, method)
        assert [opcode for opcode, _ in activation] == [
            METHOD_ACTIVATE, METHOD_CAUSE, METHOD_SURROUNDING, METHOD_CONTENT, METHOD_DONE]
        assert struct.unpack("=I", activation[1][1]) == (1,)
        assert string(activation[2][1])[0] == surrounding
        assert struct.unpack("=II", activation[2][1][-8:]) == (3, 1)
        assert struct.unpack("=II", activation[3][1]) == (1, 0)

        ime.send(method, METHOD_STRING, string_argument("字"))
        ime.send(method, METHOD_PREEDIT, string_argument("文") + struct.pack("=ii", 0, 3))
        send(ime, method, METHOD_DELETE, 2, 1)
        ime.sync()
        assert not events_for(app, text)
        send(ime, method, METHOD_COMMIT, 1)
        ime.sync()
        edits = events_for(app, text)
        assert [opcode for opcode, _ in edits] == [
            TEXT_DELETE, TEXT_STRING, TEXT_PREEDIT, TEXT_DONE]
        assert struct.unpack("=II", edits[0][1]) == (2, 1)
        assert string(edits[1][1])[0] == "字"
        assert string(edits[2][1])[0] == "文"
        assert struct.unpack("=I", edits[3][1]) == (1,)

        compositor = ime.bind("wl_compositor")
        ime.shm = ime.bind("wl_shm")
        before = query_tree(x11, root)
        surface = create_surface(ime, compositor)
        popup = ime.allocate_id()
        send(ime, method, METHOD_POPUP, popup, surface)
        popup_events = [(opcode, payload) for target, opcode, payload in
                        publish(ime, surface, solid(BLUE, 16, 12)) if target == popup]
        assert popup_events and popup_events[-1][0] == POPUP_RECTANGLE
        assert struct.unpack("=iiii", popup_events[-1][1]) == (0, -CURSOR[3], *CURSOR[2:])
        popup_window, = query_tree(x11, root) - before
        assert map_state(x11, popup_window) == VIEWABLE

        grab = ime.allocate_id()
        send(ime, method, METHOD_GRAB, grab)
        assert {opcode for opcode, _ in events_for(ime, grab)} == {
            GRAB_KEYMAP, GRAB_MODIFIERS, GRAB_REPEAT}
        assert len(ime.fds) == 1
        os.close(ime.fds.pop())
        actor.repeat(False)
        assert events_for(ime, grab) == [
            (GRAB_REPEAT, struct.pack("=ii", 0, TEST_REPEAT_DELAY))]
        actor.fake(KEY_PRESS, KEY_A + KEYCODE_OFFSET)
        actor.fake(KEY_RELEASE, KEY_A + KEYCODE_OFFSET)
        actor.sync()
        received = [struct.unpack("=IIII", payload)[2:]
                    for opcode, payload in events_for(ime, grab) if opcode == GRAB_KEY]
        assert received == [(KEY_A, 1), (KEY_A, 0)]
        assert not any(obj == app.keyboard and opcode == KEYBOARD_KEY
                       for obj, opcode, _ in app.roundtrip_events())

        send(app, text, TEXT_DISABLE)
        send(app, text, TEXT_COMMIT)
        app.sync()
        assert events_for(ime, method) == [(METHOD_DEACTIVATE, b""), (METHOD_DONE, b"")]
        assert map_state(x11, popup_window) == UNMAPPED
        send(app, text, TEXT_ENABLE)
        send(app, text, TEXT_COMMIT)
        app.sync()
        assert [opcode for opcode, _ in events_for(ime, method)] == [
            METHOD_ACTIVATE, METHOD_CAUSE, METHOD_DONE]
        assert map_state(x11, popup_window) == VIEWABLE
        ime.send(method, METHOD_STRING, string_argument("stale"))
        send(ime, method, METHOD_COMMIT, 1)
        ime.sync()
        assert not events_for(app, text)
        send(ime, grab, GRAB_RELEASE)
        ime.sync()
        actor.fake(KEY_PRESS, KEY_A + KEYCODE_OFFSET)
        actor.fake(KEY_RELEASE, KEY_A + KEYCODE_OFFSET)
        data, _ = app.until(app.keyboard, KEYBOARD_KEY)
        assert struct.unpack("=IIII", data)[2:] == (KEY_A, 1)

        actor.focus(root)
        payload, _ = app.until(text, TEXT_LEAVE)
        assert struct.unpack("=I", payload) == (app.surface,)
        assert events_for(ime, method) == [(METHOD_DEACTIVATE, b""), (METHOD_DONE, b"")]
        assert map_state(x11, popup_window) == UNMAPPED
        send(ime, method, METHOD_DESTROY)
        ime.sync()
    finally:
        ime.close()
        app.close()
        x11.close()


def main():
    host, module = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="meatball-text-input-") as directory:
        path = Path(directory) / "wayland"
        with server(host, module, {"MEATBALL_TEST_SOCKET": str(path)},
                    arguments=["+extension", "XTEST"]) as (process, display):
            wait_socket(path, process)
            check(path, display)
            assert process.poll() is None


if __name__ == "__main__":
    main()
