"""Track surface/output membership through native movement and surface commits."""

from pathlib import Path
import signal
import struct
import sys
import tempfile

from host_support import connect_x11, receive_exact, server
from native_input import XInput, MOTION, POINTER_ENTER, GET_INPUT_FOCUS, X11_REPLY_SIZE
from native_lifecycle import wait_socket
from native_outputs import Outputs, SPLIT, CONNECTION, POSITION, DESTROY_SECOND, MODE
from native_subsurfaces import child, publish, solid, RED, BLUE
from native_surfaces import root_window, query_tree, create_surface
from native_xdg import XdgClient

SURFACE_ENTER = 0
SURFACE_LEAVE = 1
SURFACE_DESTROY = 0
SURFACE_ATTACH = 1
SURFACE_COMMIT = 6
CURSOR_SET = 0
OUTPUT_RELEASE = 0
MAP_WINDOW = 8
UNMAP_WINDOW = 10
CONFIGURE_WINDOW = 12
CW_POSITION = (1 << 0) | (1 << 1)
SURFACE_SIZE = 20
STRADDLE_X = 50
SECOND_X = 80
SURFACE_Y = 10
OUTPUT_EDGE = 64
ROOT_SIZE = OUTPUT_EDGE * 2
CURSOR_HOTSPOT = SURFACE_SIZE // 2
CURSOR_FIRST_X = CURSOR_HOTSPOT
CURSOR_SECOND_X = 90
CURSOR_Y = 40
CURSOR_CHILD_X = -(CURSOR_SECOND_X - CURSOR_HOTSPOT)
SEAT_GET_POINTER = 0
DISPLAY_ID = 1
REGISTRY_GLOBAL_REMOVE = 1


class OutputSurfaces(XdgClient, Outputs):
    def __init__(self, path):
        super().__init__(path)
        self.subcompositor = self.bind("wl_subcompositor")
        self.test = self.bind("meatball_output_test")

    def settled(self):
        return self.roundtrip_events() + self.roundtrip_events()


def transitions(events, surface):
    return [(opcode, struct.unpack("=I", data)[0])
            for object_id, opcode, data in events
            if object_id == surface and opcode in (SURFACE_ENTER, SURFACE_LEAVE)]


def move(x11, window, x):
    x11.sendall(struct.pack("<BBHIHHii", CONFIGURE_WINDOW, 0, 5,
                            window, CW_POSITION, 0, x, SURFACE_Y))
    x11.sendall(struct.pack("<BBH", GET_INPUT_FOCUS, 0, 1))
    assert receive_exact(x11, X11_REPLY_SIZE)[0] == 1


def check_surfaces(path, display):
    x11, setup = connect_x11(display, return_setup=True)
    client = OutputSurfaces(path)
    try:
        root = root_window(setup)
        window, = query_tree(x11, root)
        client.change_outputs(SPLIT)
        first_name, second_name = client.outputs()
        first, _ = client.bind_output(first_name)
        second, _ = client.bind_output(second_name)
        client.commit()
        client.configure()
        events = publish(client, client.surface, solid(RED, SURFACE_SIZE, SURFACE_SIZE)) + client.settled()
        assert transitions(events, client.surface) == [(SURFACE_ENTER, first)]
        assert transitions(client.settled(), client.surface) == []
        move(x11, window, STRADDLE_X)
        assert transitions(client.settled(), client.surface) == [(SURFACE_ENTER, second)]
        move(x11, window, SECOND_X)
        assert transitions(client.settled(), client.surface) == [(SURFACE_LEAVE, first)]
        # Binding an output after mapping enters every binding owned by this peer.
        duplicate, events = client.bind_output(second_name)
        events += client.settled()
        assert transitions(events, client.surface) == [(SURFACE_ENTER, duplicate)]
        client.send(duplicate, OUTPUT_RELEASE)
        assert transitions(client.settled(), client.surface) == []
        x11.sendall(struct.pack("<BBHI", UNMAP_WINDOW, 0, 2, window))
        assert transitions(client.settled(), client.surface) == [(SURFACE_LEAVE, second)]
        x11.sendall(struct.pack("<BBHI", MAP_WINDOW, 0, 2, window))
        assert transitions(client.settled(), client.surface) == [(SURFACE_ENTER, second)]
        nested, role = child(client, client.surface, SURFACE_SIZE - SECOND_X, 0)
        publish(client, nested, solid(BLUE, SURFACE_SIZE, SURFACE_SIZE))
        assert transitions(client.settled(), nested) == []
        client.commit()
        events = client.settled()
        assert transitions(events, nested) == [(SURFACE_ENTER, first)]
        assert transitions(events, client.surface) == []
        # A synchronized child's detach waits for its parent's commit.
        client.send(nested, SURFACE_ATTACH, struct.pack("=Iii", 0, 0, 0))
        client.send(nested, SURFACE_COMMIT)
        assert transitions(client.settled(), nested) == []
        client.commit()
        assert transitions(client.settled(), nested) == [(SURFACE_LEAVE, first)]
        client.send(nested, SURFACE_DESTROY)
        client.sync()
        # Moving an output changes membership even while the window is stationary.
        events = client.change_outputs(POSITION, SECOND_X)
        assert transitions(events, client.surface) == [(SURFACE_ENTER, first)]
        events = client.change_outputs(CONNECTION, 0)
        assert transitions(events, client.surface) == [(SURFACE_LEAVE, first)]
        leave_index = next(i for i, event in enumerate(events)
                           if event[0] == client.surface and event[1] == SURFACE_LEAVE)
        remove_index = next(i for i, event in enumerate(events)
                            if event[0] not in (DISPLAY_ID, client.surface) and event[1] == REGISTRY_GLOBAL_REMOVE and len(event[2]) == 4
                            and struct.unpack("=I", event[2])[0] == first_name)
        assert leave_index < remove_index, (leave_index, remove_index, events)
        client.send(client.surface, SURFACE_ATTACH, struct.pack("=Iii", 0, 0, 0))
        client.commit()
        assert transitions(client.settled(), client.surface) == [(SURFACE_LEAVE, second)]
        client.commit()
        client.configure()
        events = publish(client, client.surface, solid(RED, SURFACE_SIZE, SURFACE_SIZE)) + client.settled()
        assert transitions(events, client.surface) == [(SURFACE_ENTER, second)]
        client.change_outputs(DESTROY_SECOND)
        assert client.outputs() == []
        client.close()
        client = None
    finally:
        if client:
            client.close()
        x11.close()


def check_cursors(path, display):
    x11, setup = connect_x11(display, return_setup=True)
    client = OutputSurfaces(path)
    try:
        root = root_window(setup)
        actor = XInput(x11, root)
        client.change_outputs(SPLIT)
        first_name, second_name = client.outputs()
        first, _ = client.bind_output(first_name)
        second, _ = client.bind_output(second_name)
        seat = client.bind("wl_seat")
        pointer = client.allocate_id()
        client.send(seat, SEAT_GET_POINTER, struct.pack("=I", pointer))
        client.commit()
        client.configure()
        events = publish(client, client.surface, solid(RED, ROOT_SIZE, ROOT_SIZE)) + client.settled()
        actor.fake(MOTION, x=CURSOR_FIRST_X, y=CURSOR_Y)
        actor.sync()
        events += client.settled()
        enters = [data for object_id, opcode, data in events
                  if object_id == pointer and opcode == POINTER_ENTER]
        assert enters
        serial, = struct.unpack_from("=I", enters[-1])
        cursor = create_surface(client, client.compositor)
        events = publish(client, cursor, solid(BLUE, SURFACE_SIZE, SURFACE_SIZE)) + client.settled()
        assert transitions(events, cursor) == []
        client.send(pointer, CURSOR_SET,
                    struct.pack("=IIii", serial, cursor, CURSOR_HOTSPOT, CURSOR_HOTSPOT))
        assert transitions(client.settled(), cursor) == [(SURFACE_ENTER, first)]
        actor.fake(MOTION, x=OUTPUT_EDGE, y=CURSOR_Y)
        actor.sync()
        assert transitions(client.settled(), cursor) == [(SURFACE_ENTER, second)]
        actor.fake(MOTION, x=CURSOR_SECOND_X, y=CURSOR_Y)
        actor.sync()
        assert transitions(client.settled(), cursor) == [(SURFACE_LEAVE, first)]
        nested, role = child(client, cursor, CURSOR_CHILD_X, 0)
        events = publish(client, nested, solid(BLUE, SURFACE_SIZE, SURFACE_SIZE)) + client.settled()
        assert transitions(events, nested) == []
        client.send(cursor, SURFACE_COMMIT)
        events = client.settled()
        assert transitions(events, cursor) == []
        assert transitions(events, nested) == [(SURFACE_ENTER, first)]
        client.send(pointer, CURSOR_SET, struct.pack("=IIii", serial, 0, 0, 0))
        events = client.settled()
        assert transitions(events, cursor) == [(SURFACE_LEAVE, second)]
        assert transitions(events, nested) == [(SURFACE_LEAVE, first)]
        client.send(pointer, CURSOR_SET,
                    struct.pack("=IIii", serial, cursor, CURSOR_HOTSPOT, CURSOR_HOTSPOT))
        events = client.settled()
        assert transitions(events, cursor) == [(SURFACE_ENTER, second)]
        assert transitions(events, nested) == [(SURFACE_ENTER, first)]
        client.send(cursor, SURFACE_DESTROY)
        events = client.settled()
        assert transitions(events, nested) == [(SURFACE_LEAVE, first)]
        client.send(nested, SURFACE_DESTROY)
        client.sync()
    finally:
        client.close()
        x11.close()


def check_screens(path, display):
    x11, setup = connect_x11(display, return_setup=True)
    client = OutputSurfaces(path)
    try:
        outputs = []
        for name in client.outputs():
            resource, events = client.bind_output(name)
            mode = [struct.unpack("=Iiii", data) for object_id, opcode, data in events
                    if object_id == resource and opcode == MODE][-1]
            outputs.append((mode[1:3], resource))
        own = [resource for size, resource in outputs if size == (ROOT_SIZE, ROOT_SIZE)]
        assert len(outputs) == 2 and len(own) == 1
        client.commit()
        client.configure()
        events = publish(client, client.surface, solid(RED, SURFACE_SIZE, SURFACE_SIZE)) + client.settled()
        assert transitions(events, client.surface) == [(SURFACE_ENTER, own[0])]
    finally:
        client.close()
        x11.close()


def check_reset(path, display, process):
    x11, setup = connect_x11(display, return_setup=True)
    client = OutputSurfaces(path)
    try:
        window, = query_tree(x11, root_window(setup))
        client.change_outputs(SPLIT)
        for name in client.outputs():
            client.bind_output(name)
        client.commit()
        client.configure()
        publish(client, client.surface, solid(RED, SURFACE_SIZE, SURFACE_SIZE))
        move(x11, window, STRADDLE_X)
        client.settled()
        pending, role = child(client, client.surface)
        publish(client, pending, solid(BLUE, SURFACE_SIZE, SURFACE_SIZE))
        process.send_signal(signal.SIGHUP)
        assert x11.recv(1) == b""
        client.disconnected()
    finally:
        client.close()
        x11.close()
    with connect_x11(display):
        wait_socket(path, process)
        fresh = Outputs(path)
        try:
            assert len(fresh.outputs()) == 1
            for name in fresh.outputs():
                fresh.bind_output(name)
        finally:
            fresh.close()


def main():
    host, probe = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="meatball-output-surfaces-") as directory:
        path = Path(directory) / "wayland"
        for check in (check_surfaces, check_cursors):
            with server(host, probe, {"MEATBALL_TEST_SOCKET": str(path)},
                        arguments=["-crtcs", "1@128x128", "+extension", "XTEST"]) as (process, display):
                wait_socket(path, process)
                check(path, display)
                assert process.poll() is None
        with server(host, probe, {"MEATBALL_TEST_SOCKET": str(path)},
                    arguments=["-crtcs", "1@128x128", "-screen", "1", "96x64x24",
                               "-crtcs", "1@96x64", "-extension", "XINERAMA"]) as (process, display):
            wait_socket(path, process)
            check_screens(path, display)
        with server(host, probe, {"MEATBALL_TEST_SOCKET": str(path)},
                    arguments=["-crtcs", "1@128x128"]) as (process, display):
            wait_socket(path, process)
            check_reset(path, display, process)
            assert process.poll() is None


if __name__ == "__main__":
    main()
