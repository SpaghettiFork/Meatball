"""Check native geometry notifications during client repaints."""

from pathlib import Path
import struct
import sys
import tempfile

from host_support import connect_x11, receive_exact, server
from native_clients import intern_atom, XA_ATOM
from native_input import (XInput, InputClient, map_window, GET_INPUT_FOCUS,
                          X11_REPLY_SIZE, MOTION, BUTTON_PRESS, BUTTON_RELEASE)
from native_lifecycle import wait_socket
from native_subsurfaces import (publish, solid, BLUE, RED, child,
                                DESTROY, ATTACH, COMMIT)
from native_surfaces import root_window, query_tree
from native_transforms import dimensions, wait_image
from native_xdg import (XDG_GEOMETRY, TOP_MIN_SIZE, TOP_MAX_SIZE,
                        CHANGE_ATTRIBUTES, CW_EVENT_MASK, CHANGE_PROPERTY,
                        SEND_EVENT_FLAG)

QUERY_EXTENSION = 98
SHAPE_SELECT_INPUT = 6
PROPERTY_CHANGE_MASK = 1 << 22
STRUCTURE_NOTIFY_MASK = 1 << 17
PROPERTY_NOTIFY = 28
CONFIGURE_NOTIFY = 22
SHAPE_BOUNDING = 0
SHAPE_INPUT = 2
SURFACE_INPUT = 5
CREATE_REGION = 1
REGION_ADD = 1
REGION_DESTROY = 0
GEOMETRY = (5, 3, 50, 50)
MIN_SIZE = (20, 20)
MAX_SIZE = (100, 100)
POINTER_POSITION = 40
SCROLL_DOWN = 5
REPAINT_COUNT = 12
SURFACE_SIZE = 64
STATUS_WIDTH = 20
STATUS_HEIGHT = 25
STATUS_Y = SURFACE_SIZE - STATUS_HEIGHT + 1
PROPERTY_FORMAT = 32
PROPERTY_REPLACE = 0


class Watcher:
    def __init__(self, display, window):
        self.connection = connect_x11(display)
        name = b"SHAPE"
        padded = name + bytes(-len(name) % 4)
        self.connection.sendall(struct.pack("<BBHHH", QUERY_EXTENSION, 0,
                                            (8 + len(padded)) // 4, len(name), 0) + padded)
        reply = receive_exact(self.connection, X11_REPLY_SIZE)
        assert reply[0] == 1 and reply[8]
        self.shape_event = reply[10]
        self.properties = {name: intern_atom(self.connection, name)
                           for name in ("_GTK_FRAME_EXTENTS", "WM_NORMAL_HINTS")}
        self.connection.sendall(struct.pack("<BBHIB3x", reply[9], SHAPE_SELECT_INPUT,
                                            3, window, 1))
        self.connection.sendall(struct.pack("<BBHIII", CHANGE_ATTRIBUTES, 0, 4, window,
                                            CW_EVENT_MASK, PROPERTY_CHANGE_MASK | STRUCTURE_NOTIFY_MASK))
        assert not self.events()

    def close(self):
        self.connection.close()

    def events(self):
        self.connection.sendall(struct.pack("<BBH", GET_INPUT_FOCUS, 0, 1))
        events = []
        while True:
            event = receive_exact(self.connection, X11_REPLY_SIZE)
            assert event[0] != 0, event.hex()
            if event[0] == 1:
                return events
            events.append(event)

    def changes(self, events):
        shapes = [event[1] for event in events if event[0] == self.shape_event]
        properties = {name: sum(event[0] == PROPERTY_NOTIFY and
                               struct.unpack_from("<I", event, 8)[0] == atom
                               for event in events)
                      for name, atom in self.properties.items()}
        return shapes, properties


def input_region(client, geometry):
    region = client.allocate_id()
    client.send(client.compositor, CREATE_REGION, struct.pack("=I", region))
    client.send(region, REGION_ADD, struct.pack("=iiii", *geometry))
    client.send(client.surface, SURFACE_INPUT, struct.pack("=I", region))
    client.send(region, REGION_DESTROY)


def check_repaints(path, display):
    x11, setup = connect_x11(display, return_setup=True)
    root = root_window(setup)
    before = query_tree(x11, root)
    client = InputClient(path)
    watcher = None
    try:
        window, = query_tree(x11, root) - before
        actor = XInput(x11, root)
        client.send(client.xdg, XDG_GEOMETRY, struct.pack("=iiii", *GEOMETRY))
        client.send(client.top, TOP_MIN_SIZE, struct.pack("=ii", *MIN_SIZE))
        client.send(client.top, TOP_MAX_SIZE, struct.pack("=ii", *MAX_SIZE))
        input_region(client, GEOMETRY)
        map_window(client, x11, root, window=window)
        client.sync()
        watcher = Watcher(display, window)
        for index in range(REPAINT_COUNT):
            actor.fake(MOTION, x=POINTER_POSITION + index, y=POINTER_POSITION)
            actor.fake(BUTTON_PRESS, SCROLL_DOWN)
            actor.fake(BUTTON_RELEASE, SCROLL_DOWN)
            actor.sync()
            client.send(client.xdg, XDG_GEOMETRY, struct.pack("=iiii", *GEOMETRY))
            client.send(client.top, TOP_MIN_SIZE, struct.pack("=ii", *MIN_SIZE))
            client.send(client.top, TOP_MAX_SIZE, struct.pack("=ii", *MAX_SIZE))
            input_region(client, GEOMETRY)
            publish(client, client.surface, solid(BLUE if index % 2 else RED, SURFACE_SIZE, SURFACE_SIZE))
            client.sync()
            events = watcher.events()
            shapes, properties = watcher.changes(events)
            assert not shapes and not any(properties.values()), (shapes, properties)
            assert not any(event[0] == CONFIGURE_NOTIFY for event in events)
        # A changed input region still sends its notification once.
        changed = (GEOMETRY[0] + 1, *GEOMETRY[1:])
        input_region(client, changed)
        client.commit()
        client.sync()
        shapes, properties = watcher.changes(watcher.events())
        assert shapes == [SHAPE_INPUT] and not any(properties.values()), (shapes, properties)
        # Redistributing equal shadow totals changes only the frame extents.
        client.send(client.xdg, XDG_GEOMETRY, struct.pack("=iiii", *changed))
        client.commit()
        client.sync()
        shapes, properties = watcher.changes(watcher.events())
        assert shapes == [SHAPE_BOUNDING], shapes
        assert properties == {"_GTK_FRAME_EXTENTS": 1, "WM_NORMAL_HINTS": 0}, properties
        client.send(client.top, TOP_MIN_SIZE, struct.pack("=ii", MIN_SIZE[0] + 1, MIN_SIZE[1]))
        client.commit()
        client.sync()
        shapes, properties = watcher.changes(watcher.events())
        assert not shapes, shapes
        assert properties == {"_GTK_FRAME_EXTENTS": 0, "WM_NORMAL_HINTS": 1}, properties
    finally:
        if watcher:
            watcher.close()
        client.close()
        x11.close()


def check_maximized_subsurfaces(path, display, fullscreen=False):
    x11, setup = connect_x11(display, return_setup=True)
    root = root_window(setup)
    before = query_tree(x11, root)
    client = InputClient(path)
    watcher = None
    try:
        window, = query_tree(x11, root) - before
        client.send(client.xdg, XDG_GEOMETRY,
                    struct.pack("=iiii", 0, 0, SURFACE_SIZE, SURFACE_SIZE))
        map_window(client, x11, root, window=window)
        state = intern_atom(x11, "_NET_WM_STATE")
        names = (("_NET_WM_STATE_FULLSCREEN",) if fullscreen else
                 ("_NET_WM_STATE_MAXIMIZED_HORZ", "_NET_WM_STATE_MAXIMIZED_VERT"))
        maximized = [intern_atom(x11, name) for name in names]
        x11.sendall(struct.pack("<BBHIIIB3xI", CHANGE_PROPERTY, PROPERTY_REPLACE,
                                6 + len(maximized), window, state, XA_ATOM,
                                PROPERTY_FORMAT, len(maximized)) + struct.pack(f"<{len(maximized)}I", *maximized))
        client.configure()
        client.sync()
        watcher = Watcher(display, window)
        for _ in range(REPAINT_COUNT):
            status, role = child(client, client.surface, -1, STATUS_Y, desync=True)
            client.commit()
            publish(client, status, solid(BLUE, STATUS_WIDTH, STATUS_HEIGHT))
            client.sync()
            assert dimensions(x11, window) == (SURFACE_SIZE, SURFACE_SIZE)
            expected = tuple(tuple(BLUE if y >= STATUS_Y and x < STATUS_WIDTH - 1 else RED
                                   for x in range(SURFACE_SIZE)) for y in range(SURFACE_SIZE))
            wait_image(x11, window, expected)
            events = client.roundtrip_events()
            assert not any(obj == client.top and opcode == 0 for obj, opcode, _ in events), events
            changes = watcher.events()
            assert not any(event[0] & ~SEND_EVENT_FLAG == CONFIGURE_NOTIFY
                           for event in changes), [event.hex() for event in changes]
            client.send(status, ATTACH, struct.pack("=Iii", 0, 0, 0))
            client.send(status, COMMIT)
            client.send(role, DESTROY)
            client.send(status, DESTROY)
            client.sync()
            assert dimensions(x11, window) == (SURFACE_SIZE, SURFACE_SIZE)
            wait_image(x11, window, solid(RED, SURFACE_SIZE, SURFACE_SIZE))
    finally:
        if watcher:
            watcher.close()
        client.close()
        x11.close()


def main():
    host, module = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="meatball-geometry-") as directory:
        path = Path(directory) / "wayland"
        with server(host, module, {"MEATBALL_TEST_SOCKET": str(path)},
                    arguments=["+extension", "XTEST"]) as (process, display):
            wait_socket(path, process)
            check_repaints(path, display)
            check_maximized_subsurfaces(path, display)
            check_maximized_subsurfaces(path, display, fullscreen=True)
            assert process.poll() is None


if __name__ == "__main__":
    main()
