"""Check Present refresh notifications and frame callback batching."""

from pathlib import Path
import struct
import sys
import tempfile

from host_support import connect_x11, server
from native_lifecycle import QUERY_EXTENSION, wait_socket
from native_selections import XSelections
from native_surfaces import query_tree
from native_xdg import CONFIGURE_EVENT, XDG_ACK_CONFIGURE, XdgClient, make_buffer

PRESENT_SELECT_INPUT = 3
PRESENT_COMPLETE_MASK = 1 << 1
PRESENT_COMPLETE = 1
PRESENT_NOTIFY_MSC = 1
GENERIC_EVENT = 35
MICROSECONDS_PER_MILLISECOND = 1000
FAKE_REFRESH_RATE = "2"


def callback_times(client, callbacks):
    times = []
    for callback in callbacks:
        while True:
            resource, opcode, payload = client.receive()
            if resource in callbacks:
                assert resource == callback and opcode == 0
                times.append(struct.unpack("=I", payload)[0])
                break
            if resource == client.xdg and opcode == CONFIGURE_EVENT:
                client.send(client.xdg, XDG_ACK_CONFIGURE, payload)
    return times


def check_present(path, display):
    peer = XSelections(display)
    with connect_x11(display) as observer:
        before = query_tree(observer, peer.root)
    client = XdgClient(path)
    try:
        with connect_x11(display) as observer:
            window, = query_tree(observer, peer.root) - before
        name = b"Present"
        reply, _ = peer.reply(peer.send(
            QUERY_EXTENSION, payload=struct.pack("<HH", len(name), 0)
            + name + bytes(-len(name) % 4)))
        assert reply[8]
        opcode = reply[9]
        peer.next_id += 1
        event_id = peer.next_id
        peer.send(opcode, PRESENT_SELECT_INPUT,
                  struct.pack("<III", event_id, window, PRESENT_COMPLETE_MASK))
        peer.sync()

        client.commit()
        client.configure()
        first = client.frame()
        client.attach(make_buffer(client))
        client.commit()
        second = client.frame()
        client.commit()
        times = callback_times(client, (first, second))
        samples = []
        for _ in times:
            while True:
                packet, extra = peer.message()
                if packet[0] == GENERIC_EVENT:
                    break
            fields = struct.unpack("<BBHIHBBIIIQQ", packet + extra)
            _, event_opcode, _, _, evtype, kind, _, event, drawable, serial, ust, msc = fields
            assert (event_opcode, evtype, kind, event, drawable) == (
                opcode, PRESENT_COMPLETE, PRESENT_NOTIFY_MSC, event_id, window)
            assert serial and msc
            samples.append((ust // MICROSECONDS_PER_MILLISECOND & 0xffffffff, msc))
        assert [time for time, _ in samples] == times
        assert samples[0][1] < samples[1][1]

        # Destroy the client with a refresh pending and confirm another
        # connection can still dispatch requests.
        client.frame()
        client.commit()
        client.sync()
    finally:
        client.close()
        peer.sync()
        peer.close()


def main():
    host, module = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="meatball-present-") as directory:
        path = Path(directory) / "wayland"
        with server(host, module, {"MEATBALL_TEST_SOCKET": str(path)},
                    arguments=["-fakescreenfps", FAKE_REFRESH_RATE]) as (process, display):
            wait_socket(path, process)
            check_present(path, display)
            assert process.poll() is None


if __name__ == "__main__":
    main()
