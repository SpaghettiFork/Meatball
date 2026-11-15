"""Exercise native pointer routing, keyboard state, and seat resource lifetime."""

import os
from pathlib import Path
import struct
import sys
import tempfile

from host_support import connect_x11, receive_exact, server
from native_keymap import Keymaps, KEY_Y
from native_lifecycle import wait_socket
from native_subsurfaces import child, publish, solid, RED, BLUE
from native_surfaces import root_window, query_tree
from native_xdg import XdgClient

QUERY_EXTENSION = 98
SET_INPUT_FOCUS = 42
GET_INPUT_FOCUS = 43
CONFIGURE_WINDOW = 12
XTEST_FAKE_INPUT = 2
KEY_PRESS = 2
KEY_RELEASE = 3
BUTTON_PRESS = 4
BUTTON_RELEASE = 5
MOTION = 6
REVERT_TO_PARENT = 2
CW_POSITION = (1 << 0) | (1 << 1)
X11_REPLY_SIZE = 32
WINDOW_ORIGIN = 24
WINDOW_SIZE = 64
POINTER_ENTER = 0
POINTER_LEAVE = 1
POINTER_MOTION = 2
POINTER_BUTTON = 3
POINTER_AXIS = 4
POINTER_FRAME = 5
POINTER_AXIS_SOURCE = 6
POINTER_AXIS_DISCRETE = 8
KEYBOARD_KEYMAP = 0
KEYBOARD_ENTER = 1
KEYBOARD_LEAVE = 2
KEYBOARD_KEY = 3
KEYBOARD_MODIFIERS = 4
KEYBOARD_REPEAT = 5
SURFACE_INPUT = 5
COMMIT = 6
KEY_A = 30
KEY_LEFTSHIFT = 42
KEY_CAPSLOCK = 58
LOCK_MASK = 2
PEER_WINDOW_ORIGIN = 96
XKB_EXTENSION_NAME = "XKEYBOARD"
XKB_USE_EXTENSION = 0
XKB_SET_CONTROLS = 7
XKB_USE_CORE_KEYBOARD = 0x0100
XKB_REPEAT_KEYS = 1 << 0
XKB_SET_CONTROLS_SIZE = 100
XKB_CONTROLS_MASK_OFFSET = 24
XKB_REPEAT_OFFSET = 36
TEST_REPEAT_RATE = 50
TEST_REPEAT_INTERVAL = 20
TEST_REPEAT_DELAY = 300
KEYCODE_OFFSET = 8
BTN_LEFT = 0x110
SHIFT_MASK = 1
FIXED_ONE = 256
XI_CHANGE_HIERARCHY = 43
XI_SET_CLIENT_POINTER = 44
XI_ADD_MASTER = 1
XI_REMOVE_MASTER = 2
XI_FLOATING = 2
XI_ADD_MASTER_NAME = b"Meatball input test"
X11_GENERIC_EVENT = 35


def extension(x11, name):
    encoded = name.encode()
    padded = encoded + bytes(-len(encoded) % 4)
    x11.sendall(struct.pack("<BBHHH", QUERY_EXTENSION, 0, (8 + len(padded)) // 4,
                            len(encoded), 0) + padded)
    reply = receive_exact(x11, X11_REPLY_SIZE)
    assert reply[0] == 1 and reply[8], reply.hex()
    return reply[9]


class XInput:
    def __init__(self, connection, root):
        self.connection = connection
        self.root = root
        self.xtest = extension(connection, "XTEST")
        self.xi = extension(connection, "XInputExtension")
        self.xkb = extension(connection, XKB_EXTENSION_NAME)
        self.connection.sendall(struct.pack("<BBHHH", self.xkb, XKB_USE_EXTENSION, 2, 1, 0))
        reply = receive_exact(self.connection, X11_REPLY_SIZE)
        assert reply[0] == 1 and reply[1], reply.hex()

    def fake(self, type, detail=0, x=0, y=0, device=0):
        self.connection.sendall(struct.pack("<BBHBBHIIIIhhIHBB", self.xtest, XTEST_FAKE_INPUT,
                                            9, type, detail, 0, 0, self.root, 0, 0,
                                            x, y, 0, 0, 0, device))

    def focus(self, window):
        self.connection.sendall(struct.pack("<BBHII", SET_INPUT_FOCUS, REVERT_TO_PARENT,
                                            3, window, 0))

    def add_master(self):
        name = XI_ADD_MASTER_NAME
        padded = name + bytes(-len(name) % 4)
        change = struct.pack("<HHHBB", XI_ADD_MASTER, 2 + len(padded) // 4, len(name), 1, 1) + padded
        self.connection.sendall(struct.pack("<BBHBBH", self.xi, XI_CHANGE_HIERARCHY,
                                            (8 + len(change)) // 4, 1, 0, 0) + change)
        self.sync()

    def remove_master(self, pointer):
        change = struct.pack("<HHHBBHH", XI_REMOVE_MASTER, 3, pointer, XI_FLOATING, 0, 0, 0)
        self.connection.sendall(struct.pack("<BBHBBH", self.xi, XI_CHANGE_HIERARCHY, 5, 1, 0, 0) + change)
        self.sync()

    def client_pointer(self, pointer):
        self.connection.sendall(struct.pack("<BBHIHH", self.xi, XI_SET_CLIENT_POINTER, 3, 0, pointer, 0))
        self.sync()

    def repeat(self, enabled):
        request = bytearray(XKB_SET_CONTROLS_SIZE)
        struct.pack_into("<BBHH", request, 0, self.xkb, XKB_SET_CONTROLS,
                         XKB_SET_CONTROLS_SIZE // 4, XKB_USE_CORE_KEYBOARD)
        struct.pack_into("<III", request, XKB_CONTROLS_MASK_OFFSET,
                         XKB_REPEAT_KEYS, XKB_REPEAT_KEYS if enabled else 0, XKB_REPEAT_KEYS)
        struct.pack_into("<HH", request, XKB_REPEAT_OFFSET,
                         TEST_REPEAT_DELAY, TEST_REPEAT_INTERVAL)
        self.connection.sendall(request)
        self.sync()

    def sync(self):
        self.connection.sendall(struct.pack("<BBH", GET_INPUT_FOCUS, 0, 1))
        while True:
            reply = receive_exact(self.connection, X11_REPLY_SIZE)
            assert reply[0] != 0, reply.hex()
            if reply[0] == 1:
                break
            if reply[0] == X11_GENERIC_EVENT:
                receive_exact(self.connection, struct.unpack_from("<I", reply, 4)[0] * 4)


class InputClient(XdgClient):
    def __init__(self, path):
        super().__init__(path)
        self.subcompositor = self.bind("wl_subcompositor")
        self.seat = self.bind("wl_seat")
        self.pointer = self.allocate_id()
        self.keyboard = self.allocate_id()
        self.send(self.seat, 0, struct.pack("=I", self.pointer))
        self.send(self.seat, 1, struct.pack("=I", self.keyboard))
        events = self.roundtrip_events()
        capabilities = [struct.unpack("=I", data)[0] for object_id, opcode, data in events
                        if object_id == self.seat and opcode == 0]
        assert capabilities and capabilities[-1] == 3
        keymap = [struct.unpack("=II", data) for object_id, opcode, data in events
                  if object_id == self.keyboard and opcode == KEYBOARD_KEYMAP]
        assert len(keymap) == 1 and keymap[0][0] == 1 and len(self.fds) == 1
        fd = self.fds.pop(0)
        parser = Keymaps()
        try:
            assert parser.symbol(os.pread(fd, keymap[0][1], 0), KEY_Y) == ord("y")
        finally:
            parser.close()
            os.close(fd)
        repeats = [struct.unpack("=ii", data) for object_id, opcode, data in events
                   if object_id == self.keyboard and opcode == KEYBOARD_REPEAT]
        assert repeats and repeats[-1][0] > 0 and repeats[-1][1] > 0

    def until(self, resource, opcode):
        events = []
        while True:
            event = self.receive()
            events.append(event)
            if event[0] == resource and event[1] == opcode:
                return event[2], events


def map_window(client, x11, root, window=None, origin=WINDOW_ORIGIN):
    if window is None:
        window, = query_tree(x11, root)
    client.commit()
    client.configure()
    publish(client, client.surface, solid(RED, WINDOW_SIZE, WINDOW_SIZE))
    x11.sendall(struct.pack("<BBHIHHii", CONFIGURE_WINDOW, 0, 5, window,
                            CW_POSITION, 0, origin, WINDOW_ORIGIN))
    client.sync()
    return window


def check_input(path, x11, root):
    client = InputClient(path)
    actor = XInput(x11, root)
    try:
        window = map_window(client, x11, root)
        actor.fake(MOTION, x=WINDOW_ORIGIN + 5, y=WINDOW_ORIGIN + 7)
        data, _ = client.until(client.pointer, POINTER_MOTION)
        assert struct.unpack("=Iii", data)[1:] == (5 * FIXED_ONE, 7 * FIXED_ONE)
        actor.fake(BUTTON_PRESS, 1)
        data, _ = client.until(client.pointer, POINTER_BUTTON)
        assert struct.unpack("=IIII", data)[2:] == (BTN_LEFT, 1)
        actor.fake(MOTION, x=WINDOW_ORIGIN + WINDOW_SIZE + 8, y=WINDOW_ORIGIN + 7)
        data, _ = client.until(client.pointer, POINTER_MOTION)
        assert struct.unpack("=Iii", data)[1:] == ((WINDOW_SIZE + 8) * FIXED_ONE, 7 * FIXED_ONE)
        actor.fake(BUTTON_RELEASE, 1)
        data, _ = client.until(client.pointer, POINTER_BUTTON)
        assert struct.unpack("=IIII", data)[2:] == (BTN_LEFT, 0)
        actor.fake(MOTION, x=WINDOW_ORIGIN + 5, y=WINDOW_ORIGIN + 7)
        client.until(client.pointer, POINTER_ENTER)
        actor.fake(BUTTON_PRESS, 4)
        data, events = client.until(client.pointer, POINTER_AXIS)
        assert struct.unpack("=IIi", data)[1:] == (0, -10 * FIXED_ONE)
        assert any(object_id == client.pointer and opcode == POINTER_AXIS_DISCRETE
                   and struct.unpack("=Ii", payload) == (0, -1)
                   for object_id, opcode, payload in events)
        actor.fake(BUTTON_RELEASE, 4)
        actor.focus(window)
        data, _ = client.until(client.keyboard, KEYBOARD_ENTER)
        assert struct.unpack_from("=II", data)[1] == client.surface
        for enabled, expected_rate in ((True, TEST_REPEAT_RATE), (False, 0),
                                        (True, TEST_REPEAT_RATE)):
            actor.repeat(enabled)
            data, _ = client.until(client.keyboard, KEYBOARD_REPEAT)
            assert struct.unpack("=ii", data) == (expected_rate, TEST_REPEAT_DELAY)
        for type in (KEY_PRESS, KEY_RELEASE):
            actor.fake(type, KEY_CAPSLOCK + KEYCODE_OFFSET)
        while True:
            data, _ = client.until(client.keyboard, KEYBOARD_MODIFIERS)
            if struct.unpack("=IIIII", data)[3] & LOCK_MASK:
                break
        client.sync()
        for type in (KEY_PRESS, KEY_RELEASE):
            actor.fake(type, KEY_CAPSLOCK + KEYCODE_OFFSET)
        while True:
            data, _ = client.until(client.keyboard, KEYBOARD_MODIFIERS)
            if struct.unpack("=IIIII", data)[3] == 0:
                break
        client.sync()
        for type, key in ((KEY_PRESS, KEY_LEFTSHIFT), (KEY_PRESS, KEY_A),
                          (KEY_RELEASE, KEY_A), (KEY_RELEASE, KEY_LEFTSHIFT)):
            actor.fake(type, key + KEYCODE_OFFSET)
        keys, modifiers = [], 0
        while len(keys) < 4 or modifiers:
            object_id, opcode, data = client.receive()
            if object_id == client.keyboard and opcode == KEYBOARD_MODIFIERS:
                modifiers = struct.unpack("=IIIII", data)[1]
            if object_id == client.keyboard and opcode == KEYBOARD_KEY:
                _, _, key, state = struct.unpack("=IIII", data)
                if key == KEY_A and state:
                    assert modifiers & SHIFT_MASK
                keys.append((key, state))
        assert keys == [(KEY_LEFTSHIFT, 1), (KEY_A, 1), (KEY_A, 0), (KEY_LEFTSHIFT, 0)]
        actor.fake(KEY_PRESS, KEY_A + KEYCODE_OFFSET)
        client.until(client.keyboard, KEYBOARD_KEY)
        actor.focus(root)
        client.until(client.keyboard, KEYBOARD_LEAVE)
        actor.focus(window)
        data, _ = client.until(client.keyboard, KEYBOARD_ENTER)
        assert struct.unpack_from("=I", data, 8)[0] == 4
        assert struct.unpack_from("=I", data, 12)[0] == KEY_A
        actor.fake(KEY_RELEASE, KEY_A + KEYCODE_OFFSET)
        data, _ = client.until(client.keyboard, KEYBOARD_KEY)
        assert struct.unpack("=IIII", data)[2:] == (KEY_A, 0)
        surface, role = child(client, client.surface, 2, 2)
        publish(client, surface, solid(BLUE, 12, 12))
        client.commit()
        data, _ = client.until(client.pointer, POINTER_ENTER)
        assert struct.unpack("=IIii", data)[1:] == (surface, 3 * FIXED_ONE, 5 * FIXED_ONE)
        region = client.allocate_id()
        client.send(client.compositor, 1, struct.pack("=I", region))
        client.send(surface, SURFACE_INPUT, struct.pack("=I", region))
        client.send(region, 0)
        client.send(surface, COMMIT)
        client.commit()
        data, _ = client.until(client.pointer, POINTER_ENTER)
        assert struct.unpack("=IIii", data)[1] == client.surface
        client.send(surface, SURFACE_INPUT, struct.pack("=I", 0))
        client.send(surface, COMMIT)
        client.commit()
        client.until(client.pointer, POINTER_ENTER)
        actor.fake(BUTTON_PRESS, 1)
        client.until(client.pointer, POINTER_BUTTON)
        client.send(surface, 0)
        data, _ = client.until(client.pointer, POINTER_ENTER)
        assert struct.unpack("=IIii", data)[1] == client.surface
        actor.fake(BUTTON_RELEASE, 1)
        original_seat = client.globals()["wl_seat"][0]
        for _ in range(2):
            actor.add_master()
            # The newly published seat is last in registry order.
            new_seat = client.bind("wl_seat")
            data, _ = client.until(new_seat, 1)
            length, = struct.unpack_from("=I", data)
            name = data[4:4 + length - 1].decode()
            pointer_id, keyboard_id = map(int, name.removeprefix("spaghetti-seat-").split("-"))
            assert client.globals()["wl_seat"][0] != original_seat
            pointer, keyboard = client.allocate_id(), client.allocate_id()
            client.send(new_seat, 0, struct.pack("=I", pointer))
            client.send(new_seat, 1, struct.pack("=I", keyboard))
            client.until(keyboard, KEYBOARD_KEYMAP)
            actor.client_pointer(pointer_id)
            actor.fake(MOTION, x=WINDOW_ORIGIN + 9, y=WINDOW_ORIGIN + 9)
            client.until(pointer, POINTER_ENTER)
            actor.focus(window)
            client.until(keyboard, KEYBOARD_ENTER)
            actor.fake(KEY_PRESS, KEY_A + KEYCODE_OFFSET)
            data, _ = client.until(keyboard, KEYBOARD_KEY)
            assert struct.unpack("=IIII", data)[2:] == (KEY_A, 1)
            actor.fake(KEY_RELEASE, KEY_A + KEYCODE_OFFSET)
            client.until(keyboard, KEYBOARD_KEY)
            actor.remove_master(pointer_id)
            data, _ = client.until(new_seat, 0)
            assert struct.unpack("=I", data) == (0,)
            # Old seat bindings remain usable while their devices are absent.
            inert = client.allocate_id()
            client.send(new_seat, 1, struct.pack("=I", inert))
            client.sync()
            client.send(inert, 0)
            client.send(pointer, 1)
            client.send(keyboard, 0)
            client.send(new_seat, 3)
            for fd in client.fds:
                os.close(fd)
            client.fds.clear()
        before = query_tree(x11, root)
        peer = InputClient(path)
        try:
            peer_window, = query_tree(x11, root) - before
            map_window(peer, x11, root, window=peer_window, origin=PEER_WINDOW_ORIGIN)
            actor.focus(window)
            actor.fake(KEY_PRESS, KEY_A + KEYCODE_OFFSET)
            client.until(client.keyboard, KEYBOARD_KEY)
            actor.focus(peer_window)
            client.until(client.keyboard, KEYBOARD_LEAVE)
            data, _ = peer.until(peer.keyboard, KEYBOARD_ENTER)
            assert struct.unpack_from("=I", data, 8)[0] == 4
            assert struct.unpack_from("=I", data, 12)[0] == KEY_A
            actor.fake(KEY_RELEASE, KEY_A + KEYCODE_OFFSET)
            data, _ = peer.until(peer.keyboard, KEYBOARD_KEY)
            assert struct.unpack("=IIII", data)[2:] == (KEY_A, 0)
            events = client.roundtrip_events()
            assert not any(object_id == client.keyboard and opcode == KEYBOARD_KEY
                           for object_id, opcode, _ in events)
            actor.fake(MOTION, x=PEER_WINDOW_ORIGIN + 5, y=WINDOW_ORIGIN + 7)
            peer.until(peer.pointer, POINTER_ENTER)
            actor.fake(BUTTON_PRESS, 1)
            peer.until(peer.pointer, POINTER_BUTTON)
        finally:
            # Disconnect with keyboard focus and a native implicit pointer grab.
            peer.close()
        actor.fake(BUTTON_RELEASE, 1)
        actor.focus(window)
        actor.sync()
        client.sync()
        client.send(client.pointer, 1)
        client.send(client.keyboard, 0)
        client.send(client.seat, 3)
        client.sync()
    finally:
        client.close()


def main():
    host, module = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="meatball-input-") as directory:
        path = Path(directory) / "wayland"
        with server(host, module, {"MEATBALL_TEST_SOCKET": str(path)},
                    arguments=["+extension", "XTEST"]) as (process, display):
            wait_socket(path, process)
            x11, setup = connect_x11(display, return_setup=True)
            try:
                check_input(path, x11, root_window(setup))
                assert process.poll() is None
            finally:
                x11.close()


if __name__ == "__main__":
    main()
