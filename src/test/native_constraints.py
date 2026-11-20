"""Check native pointer constraints, region commits, and focus changes."""

from pathlib import Path
import struct
import sys
import tempfile
import time

from host_support import STARTUP_TIMEOUT, connect_x11, receive_exact, server
from native_clients import intern_atom
from native_input import (BUTTON_PRESS, BUTTON_RELEASE, MOTION, POINTER_BUTTON,
                          POINTER_ENTER, POINTER_MOTION, WINDOW_ORIGIN,
                          InputClient, XInput, map_window)
from native_interactive import (SUBSTRUCTURE_NOTIFY, TOP_MOVE, WM_MOVE,
                                WM_SOURCE_APPLICATION, wm_barrier, wm_grab)
from native_lifecycle import POLL_INTERVAL, wait_socket
from native_subsurfaces import child, publish, solid
from native_surfaces import (WL_COMPOSITOR_CREATE_REGION, WL_REGION_ADD,
                             WL_REGION_DESTROY, input_output_windows, root_window)
from native_xdg import CHANGE_ATTRIBUTES, CW_EVENT_MASK, wm_message
from wayland_support import WaylandProtocolError

INTERFACE = "zwp_pointer_constraints_v1"
LOCK = 1
CONFINE = 2
DESTROY = 0
CONFINE_SET_REGION = 1
ACTIVATED = 0
DEACTIVATED = 1
ONESHOT = 1
PERSISTENT = 2
ALREADY_CONSTRAINED = 1
GET_POINTER = 38
X11_REPLY_SIZE = 32
GET_POINTER_ROOT_OFFSET = 16
COMMIT = 6
SURFACE_INPUT = 5
GET_SEAT_POINTER = 0
BUTTON_LEFT = 1
ORIGIN = WINDOW_ORIGIN


def pointer_position(actor):
    actor.connection.sendall(struct.pack("<BBHI", GET_POINTER, 0, 2, actor.root))
    reply = receive_exact(actor.connection, X11_REPLY_SIZE)
    assert reply[0] == 1, reply
    return struct.unpack_from("<hh", reply, GET_POINTER_ROOT_OFFSET)


def move(actor, x, y):
    actor.fake(MOTION, x=x, y=y)
    actor.sync()


def region(client, *rectangles):
    resource = client.allocate_id()
    client.send(client.compositor, WL_COMPOSITOR_CREATE_REGION, struct.pack("=I", resource))
    for rectangle in rectangles:
        client.send(resource, WL_REGION_ADD, struct.pack("=iiii", *rectangle))
    return resource


def constraint(client, manager, opcode, lifetime, *rectangles, surface=None, pointer=None):
    area = region(client, *rectangles) if rectangles else 0
    resource = client.allocate_id()
    surface = client.surface if surface is None else surface
    pointer = client.pointer if pointer is None else pointer
    client.send(manager, opcode, struct.pack("=IIIII", resource, surface, pointer, area, lifetime))
    if area:
        client.send(area, WL_REGION_DESTROY)
    return resource


def check_lock(path, actor, root):
    move(actor, ORIGIN + 100, ORIGIN + 100)
    client = InputClient(path)
    try:
        window = map_window(client, actor.connection, root)
        actor.focus(root)
        move(actor, ORIGIN + 9, ORIGIN + 11)
        client.until(client.pointer, POINTER_ENTER)
        manager = client.bind(INTERFACE)
        locked = constraint(client, manager, LOCK, ONESHOT)
        events = client.roundtrip_events() + client.roundtrip_events()
        assert not any(event[:2] == (locked, ACTIVATED) for event in events)
        actor.focus(window)
        actor.sync()
        client.until(locked, ACTIVATED)

        move(actor, ORIGIN + 90, ORIGIN + 90)
        assert pointer_position(actor) == (ORIGIN + 9, ORIGIN + 11)
        actor.fake(BUTTON_PRESS, BUTTON_LEFT)
        actor.fake(BUTTON_RELEASE, BUTTON_LEFT)
        actor.sync()
        _, events = client.until(client.pointer, POINTER_BUTTON)
        _, release = client.until(client.pointer, POINTER_BUTTON)
        assert not any(event[:2] == (client.pointer, POINTER_MOTION)
                       for event in events + release)

        actor.focus(root)
        actor.sync()
        client.until(locked, DEACTIVATED)
        move(actor, ORIGIN + 13, ORIGIN + 15)
        actor.focus(window)
        actor.sync()
        events = client.roundtrip_events() + client.roundtrip_events()
        assert not any(event[:2] == (locked, ACTIVATED) for event in events)
        move(actor, ORIGIN + 23, ORIGIN + 25)
        assert pointer_position(actor) == (ORIGIN + 23, ORIGIN + 25)

        # An exhausted one-shot object leaves room for a replacement.
        actor.fake(BUTTON_PRESS, BUTTON_LEFT)
        client.until(client.pointer, POINTER_BUTTON)
        replacement = constraint(client, manager, LOCK, PERSISTENT)
        events = client.roundtrip_events() + client.roundtrip_events()
        assert not any(event[:2] == (replacement, ACTIVATED) for event in events)
        actor.fake(BUTTON_RELEASE, BUTTON_LEFT)
        client.until(client.pointer, POINTER_BUTTON)
        client.until(replacement, ACTIVATED)
        client.send(manager, DESTROY)
        client.send(replacement, DESTROY)
        client.sync()
        move(actor, ORIGIN + 27, ORIGIN + 29)
        assert pointer_position(actor) == (ORIGIN + 27, ORIGIN + 29)
        client.send(locked, DESTROY)
        client.sync()
    finally:
        client.close()


def check_confinement(path, actor, root):
    client = InputClient(path)
    try:
        windows = input_output_windows(actor.connection, root)
        window, = windows
        map_window(client, actor.connection, root, window=window)
        mask = region(client, (2, 2, 18, 18))
        client.send(client.surface, SURFACE_INPUT, struct.pack("=I", mask))
        client.send(mask, WL_REGION_DESTROY)
        client.commit()
        client.sync()
        manager = client.bind(INTERFACE)
        confined = constraint(client, manager, CONFINE, PERSISTENT, (7, 7, 28, 28))
        actor.focus(window)
        move(actor, ORIGIN + 10, ORIGIN + 10)
        client.until(confined, ACTIVATED)
        move(actor, ORIGIN + 45, ORIGIN + 45)
        assert pointer_position(actor) == (ORIGIN + 19, ORIGIN + 19)
        move(actor, ORIGIN, ORIGIN)
        assert pointer_position(actor) == (ORIGIN + 7, ORIGIN + 7)

        mask = region(client, (13, 13, 5, 5))
        client.send(confined, CONFINE_SET_REGION, struct.pack("=I", mask))
        client.send(mask, WL_REGION_DESTROY)
        client.sync()
        assert pointer_position(actor) == (ORIGIN + 7, ORIGIN + 7)
        client.commit()
        client.until(confined, DEACTIVATED)
        move(actor, ORIGIN + 15, ORIGIN + 15)
        client.until(confined, ACTIVATED)
        move(actor, ORIGIN + 50, ORIGIN + 50)
        assert pointer_position(actor) == (ORIGIN + 17, ORIGIN + 17)

        # A hole inside the bounding rectangle is still excluded.
        mask = region(client, (8, 13, 3, 5), (13, 13, 5, 5))
        client.send(confined, CONFINE_SET_REGION, struct.pack("=I", mask))
        client.send(mask, WL_REGION_DESTROY)
        client.commit()
        client.sync()
        move(actor, ORIGIN + 11, ORIGIN + 16)
        x, y = pointer_position(actor)
        assert (ORIGIN + 8 <= x < ORIGIN + 11 or ORIGIN + 13 <= x < ORIGIN + 18)
        assert y == ORIGIN + 16

        client.send(confined, DESTROY)
        client.sync()
        move(actor, ORIGIN + 35, ORIGIN + 35)
        assert pointer_position(actor) == (ORIGIN + 35, ORIGIN + 35)
    finally:
        client.close()


def check_subsurface(path, actor, root):
    client = InputClient(path)
    try:
        window = map_window(client, actor.connection, root)
        child_surface, _ = child(client, client.surface, 9, 8)
        publish(client, child_surface, solid(0xff408080, 22, 20))
        client.commit()
        client.sync()
        manager = client.bind(INTERFACE)
        confined = constraint(client, manager, CONFINE, PERSISTENT,
                              (2, 2, 7, 8), surface=child_surface)
        actor.focus(window)
        move(actor, ORIGIN + 13, ORIGIN + 12)
        client.until(confined, ACTIVATED)
        move(actor, ORIGIN + 50, ORIGIN + 50)
        assert pointer_position(actor) == (ORIGIN + 17, ORIGIN + 17)

        empty = region(client)
        client.send(confined, CONFINE_SET_REGION, struct.pack("=I", empty))
        client.send(empty, WL_REGION_DESTROY)
        client.send(child_surface, COMMIT)
        events = client.roundtrip_events() + client.roundtrip_events()
        assert not any(event[:2] == (confined, DEACTIVATED) for event in events)
        client.commit()
        client.until(confined, DEACTIVATED)
        client.send(child_surface, DESTROY)
        client.send(confined, DESTROY)
        client.sync()
    finally:
        client.close()


def check_wm_handoff(path, actor, wm, root):
    client = InputClient(path)
    try:
        window = map_window(client, actor.connection, root)
        wm.sendall(struct.pack("<BBHIII", CHANGE_ATTRIBUTES, 0, 4,
                               root, CW_EVENT_MASK, SUBSTRUCTURE_NOTIFY))
        assert not wm_barrier(wm)
        atom = intern_atom(actor.connection, "_NET_WM_MOVERESIZE")
        manager = client.bind(INTERFACE)
        confined = constraint(client, manager, CONFINE, ONESHOT)
        actor.focus(window)
        move(actor, ORIGIN + 9, ORIGIN + 11)
        client.until(confined, ACTIVATED)
        actor.fake(BUTTON_PRESS, BUTTON_LEFT)
        data, _ = client.until(client.pointer, POINTER_BUTTON)
        serial, _, _, state = struct.unpack("=IIII", data)
        assert state == 1
        client.send(client.top, TOP_MOVE, struct.pack("=II", client.seat, serial))
        client.until(confined, DEACTIVATED)
        wm_message(wm, window, atom, (ORIGIN + 9, ORIGIN + 11, WM_MOVE,
                                      BUTTON_LEFT, WM_SOURCE_APPLICATION))
        wm_grab(wm, root)
        actor.fake(BUTTON_RELEASE, BUTTON_LEFT)
        actor.sync()
    finally:
        client.close()


def check_disconnect(path, actor, root):
    client = InputClient(path)
    try:
        window = map_window(client, actor.connection, root)
        manager = client.bind(INTERFACE)
        locked = constraint(client, manager, LOCK, PERSISTENT)
        actor.focus(window)
        move(actor, ORIGIN + 6, ORIGIN + 7)
        client.until(locked, ACTIVATED)
    finally:
        client.close()
    wait_no_windows(actor, root)
    move(actor, ORIGIN + 56, ORIGIN + 57)
    assert pointer_position(actor) == (ORIGIN + 56, ORIGIN + 57)


def wait_no_windows(actor, root):
    deadline = time.monotonic() + STARTUP_TIMEOUT
    while input_output_windows(actor.connection, root):
        assert time.monotonic() < deadline, "client windows survived disconnect"
        time.sleep(POLL_INTERVAL)


def check_duplicate(path):
    client = InputClient(path)
    try:
        manager = client.bind(INTERFACE)
        other = client.allocate_id()
        client.send(client.seat, GET_SEAT_POINTER, struct.pack("=I", other))
        constraint(client, manager, LOCK, PERSISTENT)
        constraint(client, manager, CONFINE, ONESHOT, pointer=other)
        try:
            client.sync()
        except WaylandProtocolError as error:
            assert (error.object_id, error.code) == (manager, ALREADY_CONSTRAINED)
        else:
            raise AssertionError("Overlapping pointer constraints were accepted")
    finally:
        client.close()


def main():
    host, module = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="meatball-constraints-") as directory:
        path = Path(directory) / "wayland"
        with server(host, module, {"MEATBALL_TEST_SOCKET": str(path)},
                    arguments=["+extension", "XTEST"]) as (process, display):
            wait_socket(path, process)
            x11, setup = connect_x11(display, return_setup=True)
            try:
                root = root_window(setup)
                actor = XInput(x11, root)
                for check in (check_lock, check_confinement, check_subsurface, check_disconnect):
                    check(path, actor, root)
                    wait_no_windows(actor, root)
                wm = connect_x11(display)
                try:
                    check_wm_handoff(path, actor, wm, root)
                finally:
                    wm.close()
                wait_no_windows(actor, root)
                check_duplicate(path)
                assert process.poll() is None
            finally:
                x11.close()


if __name__ == "__main__":
    main()
