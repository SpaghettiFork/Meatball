"""Verify that system bell requests reach XKB with the right window."""

from pathlib import Path
import struct
import sys
import tempfile

from host_support import connect_x11, server
from native_lifecycle import QUERY_EXTENSION, wait_socket
from native_selections import XSelections
from native_surfaces import create_surface, query_tree
from native_xdg import XdgClient, make_buffer

INTERFACE = "xdg_system_bell_v1"
DESTROY = 0
RING = 1
XKB_USE_EXTENSION = 0
XKB_SELECT_EVENTS = 1
XKB_USE_CORE_KEYBOARD = 0x0100
XKB_BELL_NOTIFY = 8
XKB_BELL_NOTIFY_MASK = 1 << XKB_BELL_NOTIFY
KBD_FEEDBACK_CLASS = 0


def subscribe(peer):
    name = b"XKEYBOARD"
    reply, _ = peer.reply(peer.send(QUERY_EXTENSION, payload=(
        struct.pack("<HH", len(name), 0) + name + bytes(-len(name) % 4))))
    assert reply[8]
    opcode, event_base = reply[9:11]
    reply, _ = peer.reply(peer.send(opcode, XKB_USE_EXTENSION,
                                    struct.pack("<HH", 1, 0)))
    assert reply[1]
    peer.send(opcode, XKB_SELECT_EVENTS, struct.pack(
        "<HHHHHH", XKB_USE_CORE_KEYBOARD, XKB_BELL_NOTIFY_MASK, 0,
        XKB_BELL_NOTIFY_MASK, 0, 0))
    peer.sync()
    return event_base


def check_bell(path, display):
    peer = XSelections(display)
    try:
        event_base = subscribe(peer)
        with connect_x11(display) as observer:
            before = query_tree(observer, peer.root)
        client = XdgClient(path)
        try:
            client.commit()
            client.configure()
            client.attach(make_buffer(client))
            client.commit()
            client.sync()
            with connect_x11(display) as observer:
                window, = query_tree(observer, peer.root) - before
            bell = client.bind(INTERFACE, 1)
            for surface, expected_window in ((client.surface, window), (0, 0)):
                client.send(bell, RING, struct.pack("=I", surface))
                event = peer.event(event_base)
                event_type, bell_class, associated_window, event_only = (
                    event[1], event[9], struct.unpack_from("<I", event, 20)[0],
                    event[24])
                assert (event_type, bell_class, associated_window, event_only) == (
                    XKB_BELL_NOTIFY, KBD_FEEDBACK_CLASS, expected_window, 0)

            roleless = create_surface(client, client.compositor)
            client.send(bell, RING, struct.pack("=I", roleless))
            client.send(bell, DESTROY)
            client.sync()
            peer.sync()
            assert not any(event[0] == event_base and event[1] == XKB_BELL_NOTIFY
                           for event in peer.events)
        finally:
            client.close()
    finally:
        peer.close()


def main():
    host, module = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="meatball-bell-") as directory:
        path = Path(directory) / "wayland"
        with server(host, module, {"MEATBALL_TEST_SOCKET": str(path)}) as (process, display):
            wait_socket(path, process)
            check_bell(path, display)
            assert process.poll() is None


if __name__ == "__main__":
    main()
