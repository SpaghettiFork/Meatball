"""Check Present pixmap copies, their pixels, and frame callback batching."""

from pathlib import Path
import struct
import sys
import tempfile

from host_support import connect_x11, server
from native_lifecycle import QUERY_EXTENSION, wait_socket
from native_selections import XSelections
from native_shm import COLORS, RGB_MASK, get_image
from native_surfaces import query_tree
from native_xdg import CONFIGURE_EVENT, XDG_ACK_CONFIGURE, XdgClient, make_buffer
from wayland_support import WaylandProtocolError

PRESENT_SELECT_INPUT = 3
PRESENT_COMPLETE_MASK = 1 << 1
PRESENT_COMPLETE = 1
PRESENT_PIXMAP = 0
PRESENT_COPY = 0
GENERIC_EVENT = 35
MICROSECONDS_PER_MILLISECOND = 1000
FAKE_REFRESH_RATE = "2"
TEARING_MANAGER = "wp_tearing_control_manager_v1"
GET_TEARING_CONTROL = 1
SET_PRESENTATION_HINT = 0
HINT_ASYNC = 1
DESTROY_TEARING_MANAGER = 0
DESTROY_TEARING_CONTROL = 1
TEARING_CONTROL_EXISTS = 0


def callback_times(client, callbacks):
    for callback in callbacks:
        while True:
            resource, opcode, payload = client.receive()
            if resource in callbacks:
                assert resource == callback and opcode == 0
                yield struct.unpack("=I", payload)[0]
                break
            if resource == client.xdg and opcode == CONFIGURE_EVENT:
                client.send(client.xdg, XDG_ACK_CONFIGURE, payload)


def frame_msc(peer, opcode, event_id, window, time):
    while True:
        packet, extra = peer.message()
        if packet[0] == GENERIC_EVENT:
            break
    fields = struct.unpack("<BBHIHBBIIIQQ", packet + extra)
    _, event_opcode, _, _, evtype, kind, mode, event, drawable, serial, ust, msc = fields
    assert (event_opcode, evtype, kind, mode, event, drawable) == (
        opcode, PRESENT_COMPLETE, PRESENT_PIXMAP, PRESENT_COPY, event_id, window)
    assert serial and msc
    assert ust // MICROSECONDS_PER_MILLISECOND & 0xffffffff == time
    return msc


def get_tearing_control(client, manager):
    control = client.allocate_id()
    client.send(manager, GET_TEARING_CONTROL, struct.pack("=II", control, client.surface))
    return control


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
        next_colors = COLORS[::-1]
        client.attach(make_buffer(client, next_colors))
        client.commit()
        samples = []
        for time, colors in zip(callback_times(client, (first, second)),
                                (COLORS, next_colors)):
            with connect_x11(display) as observer:
                assert get_image(observer, window) == tuple(color & RGB_MASK for color in colors)
            samples.append(frame_msc(peer, opcode, event_id, window, time))
        assert samples[0] < samples[1]

        manager = client.bind(TEARING_MANAGER, 1)
        control = get_tearing_control(client, manager)
        client.send(manager, DESTROY_TEARING_MANAGER)
        client.send(control, SET_PRESENTATION_HINT, struct.pack("=I", HINT_ASYNC))
        async_samples = []
        for _ in range(2):
            callback = client.frame()
            client.commit()
            time, = callback_times(client, (callback,))
            async_samples.append(frame_msc(peer, opcode, event_id, window, time))
        assert async_samples[0] == async_samples[1], async_samples

        client.send(control, DESTROY_TEARING_CONTROL)
        callback = client.frame()
        client.commit()
        time, = callback_times(client, (callback,))
        assert frame_msc(peer, opcode, event_id, window, time) > async_samples[-1]

        # Destroy the client with a refresh pending and confirm another
        # connection can still dispatch requests.
        client.frame()
        client.commit()
        client.sync()
    finally:
        client.close()
        peer.sync()
        peer.close()


def check_duplicate_tearing_control(path):
    client = XdgClient(path)
    try:
        manager = client.bind(TEARING_MANAGER, 1)
        get_tearing_control(client, manager)
        get_tearing_control(client, manager)
        try:
            client.sync()
        except WaylandProtocolError as error:
            assert (error.object_id, error.code) == (manager, TEARING_CONTROL_EXISTS)
        else:
            raise AssertionError("Multiple tearing controls were accepted")
    finally:
        client.close()


def main():
    host, module = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="meatball-present-") as directory:
        path = Path(directory) / "wayland"
        with server(host, module, {"MEATBALL_TEST_SOCKET": str(path)},
                    arguments=["-fakescreenfps", FAKE_REFRESH_RATE]) as (process, display):
            wait_socket(path, process)
            check_present(path, display)
            check_duplicate_tearing_control(path)
            assert process.poll() is None


if __name__ == "__main__":
    main()
