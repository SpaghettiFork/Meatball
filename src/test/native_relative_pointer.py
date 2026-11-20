"""Check relative motion routing and delivery through native pointer grabs."""

from pathlib import Path
import struct
import sys
import tempfile

from host_support import connect_x11, receive_exact, server
from native_clients import intern_atom
from native_constraints import (ACTIVATED, CONFINE, DESTROY, LOCK, PERSISTENT,
                                INTERFACE as CONSTRAINTS, constraint, move,
                                pointer_position, wait_no_windows)
from native_input import (BUTTON_PRESS, BUTTON_RELEASE, FIXED_ONE, MOTION,
                          PEER_WINDOW_ORIGIN, POINTER_AXIS, POINTER_ENTER, POINTER_FRAME,
                          POINTER_LEAVE, POINTER_MOTION, WINDOW_ORIGIN,
                          WINDOW_SIZE, X11_REPLY_SIZE, InputClient, XInput,
                          map_window)
from native_interactive import wm_grab
from native_lifecycle import wait_socket
from native_surfaces import input_output_windows, root_window

INTERFACE = "zwp_relative_pointer_manager_v1"
GET_RELATIVE_POINTER = 1
RELATIVE_MOTION = 0
XTEST_RELATIVE = 1
GET_SEAT_POINTER = 0
POINTER_RELEASE = 1
SEAT_CAPABILITIES = 0
SEAT_NAME = 1
SCROLL_UP = 4
XI_QUERY_DEVICE = 48
XI_CHANGE_PROPERTY = 57
XI_ALL_DEVICES = 0
XI_SLAVE_POINTER = 3
PROPERTY_REPLACE = 0


def relative_pointer(client, manager, pointer):
    resource = client.allocate_id()
    client.send(manager, GET_RELATIVE_POINTER, struct.pack("=II", resource, pointer))
    return resource


def motion(actor, dx, dy):
    actor.fake(MOTION, XTEST_RELATIVE, x=dx, y=dy)
    actor.sync()


def read_motion(client, resource, dx, dy, unaccelerated=None):
    payload, events = client.until(resource, RELATIVE_MOTION)
    high, low, *deltas = struct.unpack("=IIiiii", payload)
    raw = (dx, dy) if unaccelerated is None else unaccelerated
    assert deltas == [value * FIXED_ONE for value in (dx, dy, *raw)], deltas
    timestamp = (high << 32) | low
    assert timestamp > 0
    return timestamp, events


def assert_no_motion(events, *resources):
    assert not any(object_id in resources and opcode == RELATIVE_MOTION
                   for object_id, opcode, _ in events), events


def set_test_transform(actor, x_scale, y_scale):
    connection = actor.connection
    connection.sendall(struct.pack("<BBHHH", actor.xi, XI_QUERY_DEVICE, 2,
                                   XI_ALL_DEVICES, 0))
    reply = receive_exact(connection, X11_REPLY_SIZE)
    assert reply[0] == 1, reply.hex()
    words, count = struct.unpack_from("<IH", reply, 4)
    devices = receive_exact(connection, words * 4)
    offset = 0
    for _ in range(count):
        device, use, _, classes, name_length = struct.unpack_from("<HHHHH", devices, offset)
        offset += struct.calcsize("<HHHHHBB")
        name = devices[offset:offset + name_length]
        offset += (name_length + 3) & ~3
        if use == XI_SLAVE_POINTER and name.endswith(b"XTEST pointer"):
            break
        for _ in range(classes):
            _, length = struct.unpack_from("<HH", devices, offset)
            offset += length * 4
    else:
        raise AssertionError("XTEST pointer is missing")

    matrix = intern_atom(connection, "Coordinate Transformation Matrix")
    float_type = intern_atom(connection, "FLOAT")
    transform = (x_scale, 0, 0, 0, y_scale, 0, 0, 0, 1)
    request = struct.pack("<BBHHBBIII9f", actor.xi, XI_CHANGE_PROPERTY,
                          5 + len(transform), device, PROPERTY_REPLACE, 32,
                          matrix, float_type, len(transform), *transform)
    connection.sendall(request)
    actor.sync()


def check_relative_motion(path, actor, root, display):
    move(actor, PEER_WINDOW_ORIGIN + WINDOW_SIZE, WINDOW_ORIGIN)
    client = InputClient(path)
    try:
        first = map_window(client, actor.connection, root)
        check_focus_and_grabs(path, actor, root, client, first, display)
    finally:
        client.close()


def check_focus_and_grabs(path, actor, root, client, first, display):
    peer = InputClient(path)
    try:
        second, = input_output_windows(actor.connection, root) - {first}
        map_window(peer, actor.connection, root, second, PEER_WINDOW_ORIGIN)
        client_manager = client.bind(INTERFACE, 1)
        peer_manager = peer.bind(INTERFACE, 1)
        relative = relative_pointer(client, client_manager, client.pointer)
        duplicate = relative_pointer(client, client_manager, client.pointer)
        peer_relative = relative_pointer(peer, peer_manager, peer.pointer)
        client.send(client_manager, DESTROY)
        client.sync()
        peer.sync()

        move(actor, WINDOW_ORIGIN + 10, WINDOW_ORIGIN + 11)
        actor.focus(first)
        client.until(client.pointer, POINTER_ENTER)
        move(actor, WINDOW_ORIGIN + 11, WINDOW_ORIGIN + 12)
        _, events = client.until(client.pointer, POINTER_MOTION)
        assert_no_motion(events, relative, duplicate)

        motion(actor, 3, -2)
        timestamp, _ = read_motion(client, relative, 3, -2)
        other_time, _ = read_motion(client, duplicate, 3, -2)
        assert other_time == timestamp
        assert_no_motion(peer.roundtrip_events(), peer_relative)
        client.send(duplicate, DESTROY)
        client.sync()

        actor.fake(BUTTON_PRESS, SCROLL_UP)
        actor.fake(BUTTON_RELEASE, SCROLL_UP)
        _, events = client.until(client.pointer, POINTER_AXIS)
        assert_no_motion(events + client.roundtrip_events(), relative)

        set_test_transform(actor, 2, 3)
        motion(actor, 1, -1)
        read_motion(client, relative, 2, -3, unaccelerated=(1, -1))
        set_test_transform(actor, 1, 1)
        move(actor, WINDOW_ORIGIN + 14, WINDOW_ORIGIN + 12)

        constraints = client.bind(CONSTRAINTS)
        locked = constraint(client, constraints, LOCK, PERSISTENT)
        client.until(locked, ACTIVATED)
        client.sync()
        before = pointer_position(actor)
        motion(actor, -20, 30)
        motion(actor, 4, -3)
        later, events = read_motion(client, relative, -20, 30)
        assert later >= timestamp
        latest, following = read_motion(client, relative, 4, -3)
        assert latest >= later
        first_frame = [event[:2] for event in following
                       if event[0] in (client.pointer, relative)]
        assert first_frame == [(client.pointer, POINTER_FRAME),
                               (relative, RELATIVE_MOTION)], first_frame
        tail = client.roundtrip_events()
        assert [event[:2] for event in tail if event[0] == client.pointer] == [
            (client.pointer, POINTER_FRAME)]
        events += following + tail
        assert not any(event[:2] == (client.pointer, POINTER_MOTION) for event in events)
        assert pointer_position(actor) == before
        client.send(locked, DESTROY)
        client.sync()

        confined = constraint(client, constraints, CONFINE, PERSISTENT,
                              (8, 8, 12, 12))
        client.until(confined, ACTIVATED)
        motion(actor, 40, 35)
        read_motion(client, relative, 40, 35)
        assert pointer_position(actor) == (WINDOW_ORIGIN + 19, WINDOW_ORIGIN + 19)
        client.send(confined, DESTROY)
        client.sync()

        move(actor, PEER_WINDOW_ORIGIN + 9, WINDOW_ORIGIN + 9)
        client.until(client.pointer, POINTER_LEAVE)
        peer.until(peer.pointer, POINTER_ENTER)
        peer.send(peer.pointer, POINTER_RELEASE)
        peer.sync()
        motion(actor, 4, 2)
        read_motion(peer, peer_relative, 4, 2)
        assert_no_motion(client.roundtrip_events(), relative, duplicate)

        wm = connect_x11(display)
        try:
            wm_grab(wm, root)
            motion(actor, 2, 3)
            assert_no_motion(peer.roundtrip_events(), peer_relative)
        finally:
            wm.close()

        peer.send(peer_relative, DESTROY)
        peer.sync()
        motion(actor, 3, 5)
        assert_no_motion(peer.roundtrip_events(), peer_relative)
    finally:
        peer.close()


def check_seat_removal(path, actor, root):
    client = InputClient(path)
    try:
        map_window(client, actor.connection, root)
        manager = client.bind(INTERFACE, 1)
        original = relative_pointer(client, manager, client.pointer)
        actor.add_master()
        seat = client.bind("wl_seat")
        data, _ = client.until(seat, SEAT_NAME)
        length, = struct.unpack_from("=I", data)
        name = data[4:4 + length - 1].decode()
        pointer_id, _ = map(int, name.removeprefix("spaghetti-seat-").split("-"))
        pointer = client.allocate_id()
        client.send(seat, GET_SEAT_POINTER, struct.pack("=I", pointer))
        relative = relative_pointer(client, manager, pointer)
        client.sync()
        actor.client_pointer(pointer_id)
        move(actor, WINDOW_ORIGIN + 9, WINDOW_ORIGIN + 9)
        client.until(pointer, POINTER_ENTER)
        motion(actor, 2, 4)
        _, events = read_motion(client, relative, 2, 4)
        assert_no_motion(events + client.roundtrip_events(), original)
        actor.remove_master(pointer_id)
        assert client.until(seat, SEAT_CAPABILITIES)[0] == struct.pack("=I", 0)
        move(actor, WINDOW_ORIGIN + 10, WINDOW_ORIGIN + 10)
        motion(actor, 2, -4)
        _, events = read_motion(client, original, 2, -4)
        assert_no_motion(events + client.roundtrip_events(), relative)
        client.send(relative, DESTROY)
        client.sync()
    finally:
        client.close()


def main():
    host, module = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="meatball-relative-pointer-") as directory:
        path = Path(directory) / "wayland"
        with server(host, module, {"MEATBALL_TEST_SOCKET": str(path)},
                    arguments=["+extension", "XTEST"]) as (process, display):
            wait_socket(path, process)
            connection, setup = connect_x11(display, return_setup=True)
            try:
                root = root_window(setup)
                actor = XInput(connection, root)
                check_relative_motion(path, actor, root, display)
                wait_no_windows(actor, root)
                check_seat_removal(path, actor, root)
                assert process.poll() is None
            finally:
                connection.close()


if __name__ == "__main__":
    main()
