"""Exercise native xdg toplevel mapping and X11 window-manager interaction."""

import os
from pathlib import Path
import signal
import struct
import sys
import tempfile

from host_support import connect_x11, receive_exact, server
from native_clients import intern_atom, property32, geometry, XA_CARDINAL, XA_ATOM
from native_lifecycle import wait_socket
from native_shm import create_pool, create_buffer, wait_pixels, COLORS, WIDTH, HEIGHT, SHM_XRGB8888
from native_surfaces import root_window, query_tree, create_surface, WL_SURFACE_ATTACH, WL_SURFACE_COMMIT, WL_SURFACE_FRAME, WL_SURFACE_DESTROY
from wayland_support import Wayland, WaylandProtocolError

XDG_GET_SURFACE = 2
XDG_DESTROY = 0
XDG_GET_TOPLEVEL = 1
XDG_GEOMETRY = 3
XDG_ACK_CONFIGURE = 4
TOP_DESTROY = 0
TOP_PARENT = 1
TOP_TITLE = 2
TOP_APP_ID = 3
TOP_MAX_SIZE = 7
TOP_MIN_SIZE = 8
TOP_MAXIMIZE = 9
TOP_FULLSCREEN = 11
TOP_CLOSE = 1
CONFIGURE_EVENT = 0
X11_REPLY_SIZE = 32
CHANGE_ATTRIBUTES = 2
GET_ATTRIBUTES = 3
MAP_WINDOW = 8
CONFIGURE_WINDOW = 12
CHANGE_PROPERTY = 18
GET_PROPERTY = 20
SEND_EVENT = 25
CLIENT_MESSAGE = 33
SEND_EVENT_FLAG = 0x80
MAP_REQUEST = 20
CONFIGURE_REQUEST = 23
SUBSTRUCTURE_REDIRECT = 1 << 20
CW_EVENT_MASK = 1 << 11
CW_WIDTH_HEIGHT = (1 << 2) | (1 << 3)
VIEWABLE = 2
UNMAPPED = 0
XA_STRING = 31
XA_WINDOW = 33
NATIVE_MAX_DIMENSION = 32767
XA_WM_SIZE_HINTS = 41
TOP_STATE_MAXIMIZED = 1
TOP_STATE_FULLSCREEN = 2
TOP_STATE_ACTIVATED = 4
TITLE = "Native Meatball test"
APP_ID = "org.spaghetti.MeatballTest"
WM_WIDTH = 20
WM_HEIGHT = 30
MOTIF_HINT_DECORATIONS = 1 << 1
CLIENT_DECORATION_HINTS = (MOTIF_HINT_DECORATIONS, 0, 0, 0, 0)


def string_argument(value):
    encoded = value.encode() + b"\0"
    return struct.pack("=I", len(encoded)) + encoded + bytes(-len(encoded) % 4)


class XdgClient(Wayland):
    def __init__(self, path):
        super().__init__(path)
        self.compositor = self.bind("wl_compositor")
        self.base = self.bind("xdg_wm_base")
        self.shm = self.bind("wl_shm")
        self.surface, self.xdg, self.top = self.add_toplevel()

    def add_toplevel(self):
        surface = create_surface(self, self.compositor)
        xdg = self.allocate_id()
        self.send(self.base, XDG_GET_SURFACE, struct.pack("=II", xdg, surface))
        top = self.allocate_id()
        self.send(xdg, XDG_GET_TOPLEVEL, struct.pack("=I", top))
        self.sync()
        return surface, xdg, top

    def configure(self, acknowledge=True, xdg=None, top=None):
        xdg = self.xdg if xdg is None else xdg
        top = self.top if top is None else top
        dimensions, states = None, ()
        while True:
            object_id, opcode, data = self.receive()
            if object_id == top and opcode == CONFIGURE_EVENT:
                width, height, length = struct.unpack_from("=iiI", data)
                dimensions = (width, height)
                states = struct.unpack_from(f"={length // 4}I", data, 12)
            elif object_id == xdg and opcode == CONFIGURE_EVENT:
                serial, = struct.unpack("=I", data)
                assert dimensions is not None
                if acknowledge:
                    self.send(xdg, XDG_ACK_CONFIGURE, struct.pack("=I", serial))
                return serial, dimensions, states

    def commit(self):
        self.send(self.surface, WL_SURFACE_COMMIT)

    def attach(self, buffer):
        self.send(self.surface, WL_SURFACE_ATTACH, struct.pack("=Iii", buffer, 0, 0))

    def frame(self):
        callback = self.allocate_id()
        self.send(self.surface, WL_SURFACE_FRAME, struct.pack("=I", callback))
        return callback

    def frame_done(self, callback):
        while True:
            object_id, opcode, data = self.receive()
            if object_id == callback:
                assert opcode == 0 and len(data) == 4
                return
            if object_id == self.xdg and opcode == CONFIGURE_EVENT:
                self.send(self.xdg, XDG_ACK_CONFIGURE, data)


def map_state(x11, window):
    x11.sendall(struct.pack("<BBHI", GET_ATTRIBUTES, 0, 2, window))
    reply = receive_exact(x11, X11_REPLY_SIZE)
    assert reply[0] == 1
    length, = struct.unpack_from("<I", reply, 4)
    receive_exact(x11, length * 4)
    return reply[26]


def property8(x11, window, name, expected_type):
    property = intern_atom(x11, name)
    x11.sendall(struct.pack("<BBHIIIII", GET_PROPERTY, 0, 6, window,
                            property, expected_type, 0, 0xffffffff))
    reply = receive_exact(x11, X11_REPLY_SIZE)
    assert reply[0] == 1 and reply[1] == 8
    length, type, remaining, items = struct.unpack_from("<IIII", reply, 4)
    assert type == expected_type and remaining == 0
    return receive_exact(x11, length * 4)[:items]


def configure_window(x11, window, width, height):
    x11.sendall(struct.pack("<BBHIHHII", CONFIGURE_WINDOW, 0, 5, window,
                            CW_WIDTH_HEIGHT, 0, width, height))


def wm_map(wm, window, observer):
    while True:
        event = receive_exact(wm, X11_REPLY_SIZE)
        type = event[0] & ~SEND_EVENT_FLAG
        if type == CONFIGURE_REQUEST:
            target, = struct.unpack_from("<I", event, 8)
            width, height = struct.unpack_from("<HH", event, 20)
            configure_window(wm, target, width, height)
        elif type == MAP_REQUEST:
            target, = struct.unpack_from("<I", event, 8)
            assert target == window
            assert property32(observer, window, "_MOTIF_WM_HINTS",
                              intern_atom(observer, "_MOTIF_WM_HINTS")) == CLIENT_DECORATION_HINTS
            wm.sendall(struct.pack("<BBHI", MAP_WINDOW, 0, 2, target))
            return


def wm_message(wm, window, atom, expected):
    while True:
        event = receive_exact(wm, X11_REPLY_SIZE)
        if event[0] & ~SEND_EVENT_FLAG == CLIENT_MESSAGE:
            target, type = struct.unpack_from("<II", event, 4)
            assert target == window and type == atom
            assert struct.unpack_from("<5I", event, 12) == expected
            return


def close_request(wm, window, protocols, delete_window):
    event = struct.pack("<BBHII5I", CLIENT_MESSAGE, 32, 0, window, protocols,
                         delete_window, 0, 0, 0, 0)
    wm.sendall(struct.pack("<BBHII", SEND_EVENT, 0, 11, window, 0) + event)


def make_buffer(client):
    stride = WIDTH * 4
    fd = os.memfd_create("meatball-xdg-test", os.MFD_CLOEXEC)
    try:
        os.ftruncate(fd, stride * HEIGHT)
        os.pwrite(fd, struct.pack(f"={len(COLORS)}I", *COLORS), 0)
        pool = create_pool(client, client.shm, fd, stride * HEIGHT)
        buffer = create_buffer(client, pool, 0, WIDTH, HEIGHT, stride, SHM_XRGB8888)
        client.send(pool, 1)
        return buffer
    finally:
        os.close(fd)


def check_mapping(x11, root, path, wm=None):
    before = query_tree(x11, root)
    client = XdgClient(path)
    try:
        window, = query_tree(x11, root) - before
        assert map_state(x11, window) == UNMAPPED
        client.send(client.top, TOP_TITLE, string_argument(TITLE))
        client.send(client.top, TOP_APP_ID, string_argument(APP_ID))
        client.send(client.top, TOP_MIN_SIZE, struct.pack("=ii", 1, 1))
        client.send(client.top, TOP_MAX_SIZE, struct.pack("=ii", 100, 80))
        client.send(client.xdg, XDG_GEOMETRY, struct.pack("=iiii", 0, 0, WIDTH, HEIGHT))
        client.commit()
        _, dimensions, states = client.configure()
        assert dimensions == (0, 0) and not states
        buffer = make_buffer(client)
        callback = client.frame()
        client.attach(buffer)
        client.commit()
        if wm:
            wm_map(wm, window, x11)
        client.frame_done(callback)
        assert map_state(x11, window) == VIEWABLE
        wait_pixels(x11, window)
        assert property8(x11, window, "_NET_WM_NAME", intern_atom(x11, "UTF8_STRING")) == TITLE.encode()
        assert property8(x11, window, "WM_CLASS", XA_STRING) == (APP_ID.encode() + b"\0") * 2
        hints = property32(x11, window, "WM_NORMAL_HINTS", XA_WM_SIZE_HINTS)
        assert len(hints) == 18 and hints[0] == (1 << 4) | (1 << 5)
        assert hints[5:9] == (1, 1, 100, 80)
        assert property32(x11, window, "_GTK_FRAME_EXTENTS", XA_CARDINAL) == (0, 0, 0, 0)
        assert property32(x11, window, "_MOTIF_WM_HINTS",
                          intern_atom(x11, "_MOTIF_WM_HINTS")) == CLIENT_DECORATION_HINTS
        if wm:
            state_atom = intern_atom(x11, "_NET_WM_STATE")
            fullscreen = intern_atom(x11, "_NET_WM_STATE_FULLSCREEN")
            horizontal = intern_atom(x11, "_NET_WM_STATE_MAXIMIZED_HORZ")
            vertical = intern_atom(x11, "_NET_WM_STATE_MAXIMIZED_VERT")
            focused = intern_atom(x11, "_NET_WM_STATE_FOCUSED")
            client.send(client.top, TOP_MAXIMIZE)
            wm_message(wm, window, state_atom, (1, horizontal, vertical, 1, 0))
            client.send(client.top, TOP_FULLSCREEN, struct.pack("=I", 0))
            wm_message(wm, window, state_atom, (1, fullscreen, 0, 1, 0))
            configure_window(wm, window, WM_WIDTH, WM_HEIGHT)
            _, dimensions, _ = client.configure()
            assert dimensions == (WM_WIDTH, WM_HEIGHT)
            states = [fullscreen, horizontal, vertical, focused]
            wm.sendall(struct.pack("<BBHIIIB3xI", CHANGE_PROPERTY, 0, 6 + len(states), window,
                                    state_atom, XA_ATOM, 32, len(states)) + struct.pack("<4I", *states))
            _, _, states = client.configure()
            assert set(states) == {TOP_STATE_MAXIMIZED, TOP_STATE_FULLSCREEN, TOP_STATE_ACTIVATED}
            close_request(wm, window, intern_atom(x11, "WM_PROTOCOLS"), intern_atom(x11, "WM_DELETE_WINDOW"))
            while True:
                object_id, opcode, data = client.receive()
                if object_id == client.top and opcode == TOP_CLOSE:
                    break
        client.attach(0)
        client.commit()
        client.sync()
        assert map_state(x11, window) == UNMAPPED
        client.commit()
        client.configure()
        client.attach(buffer)
        callback = client.frame()
        client.commit()
        if wm:
            wm_map(wm, window, x11)
        client.frame_done(callback)
        assert map_state(x11, window) == VIEWABLE
        assert property32(x11, window, "_MOTIF_WM_HINTS",
                          intern_atom(x11, "_MOTIF_WM_HINTS")) == CLIENT_DECORATION_HINTS
        client.send(client.top, TOP_DESTROY)
        client.send(client.xdg, XDG_DESTROY)
        client.send(client.surface, WL_SURFACE_DESTROY)
        client.sync()
        geometry(x11, window, False)
        assert query_tree(x11, root) == before
    finally:
        client.close()


def check_parents_and_geometry(x11, root, path):
    before = query_tree(x11, root)
    client = XdgClient(path)
    try:
        first_window, = query_tree(x11, root) - before
        client.send(client.top, TOP_MAXIMIZE)
        client.commit()
        _, _, states = client.configure()
        assert states == (TOP_STATE_MAXIMIZED,)
        state_atoms = property32(x11, first_window, "_NET_WM_STATE", XA_ATOM)
        assert set(state_atoms) == {intern_atom(x11, "_NET_WM_STATE_MAXIMIZED_HORZ"),
                                    intern_atom(x11, "_NET_WM_STATE_MAXIMIZED_VERT")}
        buffer = make_buffer(client)
        client.attach(buffer)
        callback = client.frame()
        client.commit()
        client.frame_done(callback)
        client.send(client.xdg, XDG_GEOMETRY, struct.pack("=iiii", 1, 0, 1, HEIGHT))
        client.send(client.top, TOP_MIN_SIZE, struct.pack("=ii", 1, 1))
        client.send(client.top, TOP_MAX_SIZE, struct.pack("=ii", 100, 0))
        client.sync()
        assert property32(x11, first_window, "WM_NORMAL_HINTS", XA_WM_SIZE_HINTS)[0] == 0
        client.commit()
        client.sync()
        assert property32(x11, first_window, "_GTK_FRAME_EXTENTS", XA_CARDINAL) == (1, 0, 0, 0)
        hints = property32(x11, first_window, "WM_NORMAL_HINTS", XA_WM_SIZE_HINTS)
        assert hints[5:9] == (2, 1, 101, NATIVE_MAX_DIMENSION)
        windows = {first_window}
        roles = [(client.surface, client.xdg, client.top, first_window)]
        for _ in range(2):
            surface, xdg, top = client.add_toplevel()
            window, = query_tree(x11, root) - before - windows
            windows.add(window)
            client.send(top, TOP_PARENT, struct.pack("=I", roles[-1][2]))
            client.send(surface, WL_SURFACE_COMMIT)
            client.configure(xdg=xdg, top=top)
            client.send(surface, WL_SURFACE_ATTACH, struct.pack("=Iii", buffer, 0, 0))
            callback = client.allocate_id()
            client.send(surface, WL_SURFACE_FRAME, struct.pack("=I", callback))
            client.send(surface, WL_SURFACE_COMMIT)
            client.frame_done(callback)
            assert property32(x11, window, "WM_TRANSIENT_FOR", XA_WINDOW) == (roles[-1][3],)
            roles.append((surface, xdg, top, window))
        client.send(roles[1][0], WL_SURFACE_ATTACH, struct.pack("=Iii", 0, 0, 0))
        client.send(roles[1][0], WL_SURFACE_COMMIT)
        client.sync()
        assert property32(x11, roles[2][3], "WM_TRANSIENT_FOR", XA_WINDOW) == (first_window,)
        client.send(client.top, TOP_PARENT, struct.pack("=I", roles[2][2]))
        try:
            client.sync()
        except WaylandProtocolError as error:
            assert error.object_id == client.top and error.code == 1
        else:
            raise AssertionError("Cyclic toplevel parent accepted")
    finally:
        client.close()


def check_errors(path):
    observer = Wayland(path)
    try:
        for failure in ["unconfigured", "serial", "double_ack", "role", "base", "size", "surface"]:
            client = XdgClient(path)
            try:
                target, code = client.xdg, 3
                if failure == "unconfigured":
                    client.attach(make_buffer(client))
                    client.commit()
                elif failure == "serial":
                    client.send(client.xdg, XDG_ACK_CONFIGURE, struct.pack("=I", 0))
                    code = 4
                elif failure == "double_ack":
                    client.commit()
                    serial, _, _ = client.configure()
                    client.send(client.xdg, XDG_ACK_CONFIGURE, struct.pack("=I", serial))
                    code = 4
                elif failure == "role":
                    client.send(client.xdg, XDG_DESTROY)
                    target, code = client.base, 4
                elif failure == "base":
                    client.send(client.base, XDG_DESTROY)
                    target, code = client.base, 1
                elif failure == "size":
                    client.send(client.top, TOP_MIN_SIZE, struct.pack("=ii", 100, 100))
                    client.send(client.top, TOP_MAX_SIZE, struct.pack("=ii", 10, 10))
                    client.commit()
                    target, code = client.top, 2
                elif failure == "surface":
                    client.send(client.surface, WL_SURFACE_DESTROY)
                    target, code = client.base, 4
                try:
                    client.sync()
                except WaylandProtocolError as error:
                    assert (error.object_id, error.code) == (target, code), (failure, error)
                else:
                    raise AssertionError(f"Invalid xdg request accepted: {failure}")
            finally:
                client.close()
            observer.sync()
    finally:
        observer.close()


def main():
    host, module = sys.argv[1:3]
    with tempfile.TemporaryDirectory(prefix="meatball-xdg-") as directory:
        path = Path(directory) / "wayland"
        with server(host, module, {"MEATBALL_TEST_SOCKET": str(path)}) as (process, display):
            x11, setup = connect_x11(display, return_setup=True)
            try:
                wait_socket(path, process)
                root = root_window(setup)
                check_mapping(x11, root, path)
                with connect_x11(display) as wm:
                    wm.sendall(struct.pack("<BBHIII", CHANGE_ATTRIBUTES, 0, 4,
                                            root, CW_EVENT_MASK, SUBSTRUCTURE_REDIRECT))
                    check_mapping(x11, root, path, wm)
                check_parents_and_geometry(x11, root, path)
                check_errors(path)
                client = XdgClient(path)
                client.commit()
                client.configure()
                client.attach(make_buffer(client))
                client.commit()
                client.sync()
                process.send_signal(signal.SIGHUP)
                assert x11.recv(1) == b""
                client.disconnected()
                client.close()
            finally:
                x11.close()
            with connect_x11(display) as x11:
                client = XdgClient(path)
                client.commit()
                client.configure()
                client.close()


if __name__ == "__main__":
    main()
