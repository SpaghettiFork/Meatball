"""Check smooth touchpad scrolling and discrete wheel fallback."""

from pathlib import Path
import struct
import sys
import tempfile

from host_support import connect_x11, receive_exact, server
from native_constraints import move
from native_input import (BUTTON_PRESS, BUTTON_RELEASE, FIXED_ONE,
                          POINTER_AXIS, POINTER_AXIS_DISCRETE, POINTER_AXIS_SOURCE,
                          POINTER_ENTER, POINTER_FRAME, POINTER_MOTION, QUERY_EXTENSION,
                          WINDOW_ORIGIN, X11_REPLY_SIZE, XTEST_FAKE_INPUT,
                          InputClient, XInput, map_window)
from native_lifecycle import wait_socket
from native_surfaces import root_window

TOUCHPAD_NAME = b"Meatball test touchpad"
XI_QUERY_DEVICE = 48
XI_ALL_DEVICES = 0
XI_DEVICE_MOTION = 5
POINTER_AXIS_COUNT = 4
XTEST_RELATIVE = 1
SCROLL_DOWN = 5
SOURCE_WHEEL = 0
SOURCE_CONTINUOUS = 2
VERTICAL = 0
HORIZONTAL = 1


def find_touchpad(actor):
    connection = actor.connection
    connection.sendall(struct.pack("<BBHHH", actor.xi, XI_QUERY_DEVICE, 2,
                                   XI_ALL_DEVICES, 0))
    reply = receive_exact(connection, X11_REPLY_SIZE)
    assert reply[0] == 1
    words, count = struct.unpack_from("<IH", reply, 4)
    devices = receive_exact(connection, words * 4)
    offset = 0
    for _ in range(count):
        device, _, _, classes, name_length = struct.unpack_from("<HHHHH", devices, offset)
        offset += struct.calcsize("<HHHHHBB")
        if devices[offset:offset + name_length] == TOUCHPAD_NAME:
            break
        offset += (name_length + 3) & ~3
        for _ in range(classes):
            _, length = struct.unpack_from("<HH", devices, offset)
            offset += length * 4
    else:
        raise AssertionError("Test touchpad is missing")
    name = b"XInputExtension"
    padded = name + bytes(-len(name) % 4)
    connection.sendall(struct.pack("<BBHHH", QUERY_EXTENSION, 0,
                                   (8 + len(padded)) // 4, len(name), 0) + padded)
    reply = receive_exact(connection, X11_REPLY_SIZE)
    assert reply[0] == 1 and reply[8]
    return device, reply[10]


def touchpad_scroll(actor, touchpad, event_base, dy, dx):
    event = struct.pack("<BBHBBHIIIIhhIHBB", actor.xtest, XTEST_FAKE_INPUT,
                        17, event_base + XI_DEVICE_MOTION, XTEST_RELATIVE,
                        0, 0, actor.root, 0, 0, 0, 0, 0, 0, 0, touchpad)
    valuators = struct.pack("<BBHHBB6i", event_base, touchpad, 0, 0,
                            POINTER_AXIS_COUNT, 0, 0, 0, dy, dx, 0, 0)
    actor.connection.sendall(event + valuators)
    actor.sync()


def check_scroll(path, actor, root):
    move(actor, WINDOW_ORIGIN + 100, WINDOW_ORIGIN + 100)
    touchpad, event_base = find_touchpad(actor)
    client = InputClient(path)
    try:
        map_window(client, actor.connection, root)
        move(actor, WINDOW_ORIGIN + 9, WINDOW_ORIGIN + 11)
        client.until(client.pointer, POINTER_ENTER)
        client.sync()

        for dy, dx in ((3, -5), (24, 2), (-4, 28)):
            touchpad_scroll(actor, touchpad, event_base, dy, dx)
            _, events = client.until(client.pointer, POINTER_AXIS)
            _, tail = client.until(client.pointer, POINTER_FRAME)
            events += tail + client.roundtrip_events()
            pointer_events = [(opcode, payload) for resource, opcode, payload in events
                              if resource == client.pointer]
            assert [opcode for opcode, _ in pointer_events] == [
                POINTER_AXIS_SOURCE, POINTER_AXIS, POINTER_AXIS, POINTER_FRAME]
            assert struct.unpack("=I", pointer_events[0][1]) == (SOURCE_CONTINUOUS,)
            vertical = struct.unpack("=IIi", pointer_events[1][1])
            horizontal = struct.unpack("=IIi", pointer_events[2][1])
            assert vertical == (horizontal[0], VERTICAL, dy * FIXED_ONE // 2)
            assert horizontal[1:] == (HORIZONTAL, dx * FIXED_ONE // 2)

        actor.fake(BUTTON_PRESS, SCROLL_DOWN)
        actor.fake(BUTTON_RELEASE, SCROLL_DOWN)
        _, events = client.until(client.pointer, POINTER_AXIS)
        assert any(resource == client.pointer and opcode == POINTER_AXIS_SOURCE
                   and struct.unpack("=I", payload) == (SOURCE_WHEEL,)
                   for resource, opcode, payload in events)
        assert any(resource == client.pointer and opcode == POINTER_AXIS_DISCRETE
                   and struct.unpack("=Ii", payload) == (VERTICAL, 1)
                   for resource, opcode, payload in events)
    finally:
        client.close()


def main():
    host, module = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="meatball-scroll-") as directory:
        path = Path(directory) / "wayland"
        with server(host, module, {"MEATBALL_TEST_SOCKET": str(path),
                                   "MEATBALL_TEST_TOUCHPAD": "1"},
                    arguments=["+extension", "XTEST"]) as (process, display):
            wait_socket(path, process)
            connection, setup = connect_x11(display, return_setup=True)
            with connection:
                root = root_window(setup)
                check_scroll(path, XInput(connection, root), root)
            assert process.poll() is None


if __name__ == "__main__":
    main()
