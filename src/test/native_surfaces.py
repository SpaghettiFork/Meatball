"""Check native wl_surface ownership and double-buffered input regions."""

from pathlib import Path
import signal
import struct
import sys
import tempfile

from host_support import connect_x11, receive_exact, server
from native_clients import geometry, intern_atom, KILL_CLIENT
from native_lifecycle import wait_socket, QUERY_EXTENSION
from wayland_support import Wayland

QUERY_TREE = 15
X11_REPLY_SIZE = 32
SETUP_SIZE = 32
PIXMAP_FORMAT_SIZE = 8
VENDOR_LENGTH_OFFSET = 16
FORMAT_COUNT_OFFSET = 21
SHAPE_GET_RECTANGLES = 8
SHAPE_INPUT = 2
SHAPE_BOUNDING = 0
WL_COMPOSITOR_CREATE_SURFACE = 0
WL_COMPOSITOR_CREATE_REGION = 1
WL_SURFACE_DESTROY = 0
WL_SURFACE_ATTACH = 1
WL_SURFACE_FRAME = 3
WL_SURFACE_INPUT = 5
WL_SURFACE_COMMIT = 6
WL_REGION_DESTROY = 0
WL_REGION_ADD = 1
WL_REGION_SUBTRACT = 2


def root_window(setup):
    vendor_length, = struct.unpack_from("<H", setup, VENDOR_LENGTH_OFFSET)
    offset = SETUP_SIZE + ((vendor_length + 3) & ~3) + setup[FORMAT_COUNT_OFFSET] * PIXMAP_FORMAT_SIZE
    return struct.unpack_from("<I", setup, offset)[0]


def query_tree(x11, window):
    x11.sendall(struct.pack("<BBHI", QUERY_TREE, 0, 2, window))
    reply = receive_exact(x11, X11_REPLY_SIZE)
    assert reply[0] == 1
    words, = struct.unpack_from("<I", reply, 4)
    count, = struct.unpack_from("<H", reply, 16)
    payload = receive_exact(x11, words * 4)
    return set(struct.unpack(f"<{count}I", payload))


def shape_rectangles(x11, window, kind=SHAPE_INPUT):
    name = b"SHAPE"
    padded = name + bytes(-len(name) % 4)
    x11.sendall(struct.pack("<BBHHH", QUERY_EXTENSION, 0, (8 + len(padded)) // 4,
                            len(name), 0) + padded)
    reply = receive_exact(x11, X11_REPLY_SIZE)
    assert reply[0] == 1 and reply[8]
    opcode = reply[9]
    x11.sendall(struct.pack("<BBHIB3x", opcode, SHAPE_GET_RECTANGLES, 3,
                            window, kind))
    reply = receive_exact(x11, X11_REPLY_SIZE)
    assert reply[0] == 1
    words, count = struct.unpack_from("<II", reply, 4)
    payload = receive_exact(x11, words * 4)
    return [struct.unpack_from("<hhHH", payload, offset * 8) for offset in range(count)]


def create_surface(client, compositor_id):
    surface = client.allocate_id()
    client.send(compositor_id, WL_COMPOSITOR_CREATE_SURFACE, struct.pack("=I", surface))
    client.sync()
    return surface


def check_surface(x11, root, path):
    client = Wayland(path)
    try:
        compositor_id = client.bind("wl_compositor")
        before = query_tree(x11, root)
        surface = create_surface(client, compositor_id)
        added = query_tree(x11, root) - before
        assert len(added) == 1
        window, = added
        geometry(x11, window, True)
        assert shape_rectangles(x11, window) == [(0, 0, 1, 1)]
        region = client.allocate_id()
        client.send(compositor_id, WL_COMPOSITOR_CREATE_REGION, struct.pack("=I", region))
        client.send(surface, WL_SURFACE_INPUT, struct.pack("=I", region))
        client.sync()
        assert shape_rectangles(x11, window) == [(0, 0, 1, 1)]
        client.send(surface, WL_SURFACE_COMMIT)
        client.sync()
        assert shape_rectangles(x11, window) == []
        client.send(region, WL_REGION_ADD, struct.pack("=iiii", 0, 0, 1, 1))
        client.send(surface, WL_SURFACE_COMMIT)
        client.sync()
        assert shape_rectangles(x11, window) == []
        client.send(surface, WL_SURFACE_INPUT, struct.pack("=I", region))
        client.send(region, WL_REGION_DESTROY)
        client.send(surface, WL_SURFACE_COMMIT)
        client.sync()
        assert shape_rectangles(x11, window) == [(0, 0, 1, 1)]
        client.send(surface, WL_SURFACE_INPUT, struct.pack("=I", 0))
        client.send(surface, WL_SURFACE_ATTACH, struct.pack("=Iii", 0, 0, 0))
        callback = client.allocate_id()
        client.send(surface, WL_SURFACE_FRAME, struct.pack("=I", callback))
        client.send(surface, WL_SURFACE_COMMIT)
        client.send(surface, WL_SURFACE_DESTROY)
        client.sync()
        assert query_tree(x11, root) == before
        geometry(x11, window, False)
        surface = create_surface(client, compositor_id)
        window, = query_tree(x11, root) - before
        x11.sendall(struct.pack("<BBHI", KILL_CLIENT, 0, 2, window))
        client.disconnected()
        assert query_tree(x11, root) == before
    finally:
        client.close()


def main():
    host, module = sys.argv[1:3]
    with tempfile.TemporaryDirectory(prefix="meatball-surfaces-") as directory:
        path = Path(directory) / "wayland"
        with server(host, module, {"MEATBALL_TEST_SOCKET": str(path)}) as (process, display):
            x11, setup = connect_x11(display, return_setup=True)
            try:
                wait_socket(path, process)
                root = root_window(setup)
                check_surface(x11, root, path)
                client = Wayland(path)
                compositor_id = client.bind("wl_compositor")
                create_surface(client, compositor_id)
                process.send_signal(signal.SIGHUP)
                assert x11.recv(1) == b""
                client.disconnected()
                client.close()
            finally:
                x11.close()
            x11, setup = connect_x11(display, return_setup=True)
            try:
                wait_socket(path, process)
                check_surface(x11, root_window(setup), path)
            finally:
                x11.close()


if __name__ == "__main__":
    main()
