"""Exercise clipboard conversions and INCR transfers across X11 and Wayland."""

from collections import deque
from pathlib import Path
import fcntl
import os
import select
import signal
import struct
import sys
import tempfile

from host_support import connect_x11, receive_exact, server, STARTUP_TIMEOUT
from native_data import (mapped, focus, receive_pipe, offered, MIME_TEXT,
                         MIME_BINARY, PAYLOAD, SOURCE_DESTROY, SOURCE_CANCELLED)
from native_input import XInput
from native_lifecycle import wait_socket
from native_surfaces import root_window

SELECTION_CLEAR = 29
SELECTION_REQUEST = 30
SELECTION_NOTIFY = 31
PROPERTY_NOTIFY = 28
PROPERTY_DELETE = 1
PROPERTY_CHANGE_MASK = 1 << 22
STRUCTURE_NOTIFY_MASK = 1 << 17
XA_ATOM = 4
XA_INTEGER = 19
XA_STRING = 31
LARGE_PAYLOAD = bytes(range(256)) * 4096
CHUNK_SIZE = 16384
LARGE_X_CHUNK = 128 * 1024
BLOCKED_PIPE_SIZE = 4096


class XSelections:
    def __init__(self, display):
        self.connection, setup = connect_x11(display, return_setup=True)
        self.root = root_window(setup)
        self.base, self.mask = struct.unpack_from("<II", setup, 4)
        self.next_id = self.base
        self.events = deque()
        self.sequence = 0
        self.atoms = {}
        self.window = self.create_window()
        for name in ("CLIPBOARD", "TARGETS", "TIMESTAMP", "INCR", "UTF8_STRING",
                     "TEXT", MIME_TEXT, MIME_BINARY, "_MEATBALL_TEST_TRANSFER"):
            self.atom(name)
        self.property = self.atom("_MEATBALL_TEST_TRANSFER")

    def close(self):
        self.connection.close()

    def send(self, opcode, detail=0, payload=b""):
        assert len(payload) % 4 == 0
        self.sequence += 1
        self.connection.sendall(struct.pack("<BBH", opcode, detail,
                                            1 + len(payload) // 4) + payload)
        return self.sequence

    def message(self):
        packet = receive_exact(self.connection, 32)
        assert packet[0], f"X11 error: {packet.hex()}"
        extra = b""
        if packet[0] == 1 or packet[0] & 0x7f == 35:
            extra = receive_exact(self.connection,
                                  struct.unpack_from("<I", packet, 4)[0] * 4)
        return packet, extra

    def reply(self, sequence):
        while True:
            packet, extra = self.message()
            if packet[0] == 1:
                assert struct.unpack_from("<H", packet, 2)[0] == sequence & 0xffff
                return packet, extra
            self.events.append(packet)

    def sync(self):
        self.reply(self.send(43))

    def event(self, kind, predicate=lambda event: True):
        for event in tuple(self.events):
            if event[0] & 0x7f == kind and predicate(event):
                self.events.remove(event)
                return event
        while True:
            packet, _ = self.message()
            assert packet[0] != 1
            if packet[0] & 0x7f == kind and predicate(packet):
                return packet
            self.events.append(packet)

    def atom(self, name):
        if name not in self.atoms:
            encoded = name.encode()
            packet, _ = self.reply(self.send(16, 0, struct.pack("<HH", len(encoded), 0)
                                             + encoded + bytes(-len(encoded) % 4)))
            self.atoms[name] = struct.unpack_from("<I", packet, 8)[0]
        return self.atoms[name]

    def create_window(self):
        self.next_id += 1
        window = self.next_id
        self.send(1, 0, struct.pack("<IIhhHHHHIII", window, self.root,
                                   0, 0, 1, 1, 0, 2, 0, 1 << 11,
                                   PROPERTY_CHANGE_MASK | STRUCTURE_NOTIFY_MASK))
        return window

    def watch(self, window):
        self.send(2, 0, struct.pack("<III", window, 1 << 11,
                                   PROPERTY_CHANGE_MASK | STRUCTURE_NOTIFY_MASK))

    def own(self, selection="CLIPBOARD", window=None):
        self.send(22, 0, struct.pack("<III", self.window if window is None else window,
                                    self.atom(selection), 0))
        self.sync()

    def owner(self, selection="CLIPBOARD"):
        packet, _ = self.reply(self.send(23, 0, struct.pack("<I", self.atom(selection))))
        return struct.unpack_from("<I", packet, 8)[0]

    def change(self, window, property, type, value, format=8):
        if format == 32:
            value = struct.pack(f"<{len(value)}I", *value)
        self.send(18, 0, struct.pack("<IIIB3xI", window, property, type, format,
                                     len(value) // (format // 8))
                  + value + bytes(-len(value) % 4))

    def delete(self, window, property):
        self.send(19, 0, struct.pack("<II", window, property))

    def get(self, window=None, property=None, delete=False):
        packet, extra = self.reply(self.send(20, int(delete), struct.pack(
            "<IIIII", self.window if window is None else window,
            self.property if property is None else property, 0, 0, 0x1fffffff)))
        format = packet[1]
        type, remaining, count = struct.unpack_from("<III", packet, 8)
        assert remaining == 0
        return type, format, extra[:count * (format // 8)]

    def convert(self, target, selection="CLIPBOARD", property=None):
        target = self.atom(target) if isinstance(target, str) else target
        self.delete(self.window, self.property)
        self.send(24, 0, struct.pack("<IIIII", self.window, self.atom(selection),
                                    target, self.property if property is None else property, 0))

    def result(self, selection="CLIPBOARD"):
        event = self.event(SELECTION_NOTIFY)
        _, requestor, selected, target, property = struct.unpack_from("<IIIII", event, 4)
        assert requestor == self.window and selected == self.atom(selection)
        if not property:
            return None
        # PropertyNotify for the initial result precedes SelectionNotify.
        # It cannot acknowledge a later INCR chunk.
        self.events = deque(event for event in self.events
                            if not (event[0] & 0x7f == PROPERTY_NOTIFY
                                    and struct.unpack_from("<II", event, 4)
                                    == (self.window, property)))
        type, format, data = self.get(property=property, delete=True)
        if type != self.atom("INCR"):
            return type, format, data
        assert format == 32
        result = bytearray()
        while True:
            self.event(PROPERTY_NOTIFY, lambda event:
                       struct.unpack_from("<II", event, 4) == (self.window, property)
                       and event[16] == 0)
            type, format, data = self.get(property=property, delete=True)
            if not data:
                return type, format, bytes(result)
            result.extend(data)

    def request(self, target=None):
        event = self.event(SELECTION_REQUEST)
        time, owner, window, selection, atom, property = struct.unpack_from("<IIIIII", event, 4)
        assert owner == self.window
        if target is not None:
            assert atom == self.atom(target), (atom, self.atom(target))
        return time, window, selection, atom, property

    def notify(self, request, property):
        time, window, selection, target, _ = request
        event = struct.pack("<BBHIIIII8x", SELECTION_NOTIFY, 0, 0,
                            time, window, selection, target, property)
        self.send(25, 0, struct.pack("<II", window, 0) + event)

    def answer(self, request, type, data, format=8):
        _, window, _, _, property = request
        self.change(window, property, type, data, format)
        self.notify(request, property)

    def incr(self, request, type, payload, size_hint=True, chunk=CHUNK_SIZE):
        _, window, _, _, property = request
        self.watch(window)
        self.answer(request, self.atom("INCR"), [len(payload)] if size_hint else [], 32)
        for offset in range(0, len(payload) + chunk, chunk):
            self.event(PROPERTY_NOTIFY, lambda event:
                       struct.unpack_from("<II", event, 4) == (window, property)
                       and event[16] == PROPERTY_DELETE)
            self.change(window, property, type, payload[offset:offset + chunk])


def pipe_result(reader):
    result = bytearray()
    while True:
        ready, _, _ = select.select([reader], [], [], STARTUP_TIMEOUT)
        assert ready, "selection transfer did not finish"
        data = os.read(reader, len(LARGE_PAYLOAD))
        if not data:
            return bytes(result)
        result.extend(data)


def check_clipboard(path, display):
    connection, setup = connect_x11(display, return_setup=True)
    root = root_window(setup)
    actor = XInput(connection, root)
    client, window = mapped(path, actor, root)
    peer = XSelections(display)
    try:
        serial, _ = focus(client, actor, window)
        source = client.source()
        client.set_selection(source, serial)
        client.selection()
        client.sync()
        assert peer.owner() not in (0, peer.window)
        peer.convert("TARGETS")
        type, format, data = peer.result()
        assert (type, format) == (XA_ATOM, 32)
        targets = struct.unpack(f"<{len(data) // 4}I", data)
        assert {peer.atom(MIME_TEXT), peer.atom(MIME_BINARY), peer.atom("UTF8_STRING")} <= set(targets)
        peer.convert("TIMESTAMP")
        type, format, data = peer.result()
        assert (type, format, len(data)) == (XA_INTEGER, 32, 4)
        peer.convert("application/x-unsupported")
        assert peer.result() is None
        for target in (MIME_TEXT, "UTF8_STRING", "TEXT"):
            peer.convert(target)
            mime, writer = client.send_fd(source)
            assert mime == MIME_TEXT
            os.write(writer, PAYLOAD)
            os.close(writer)
            assert peer.result() == (peer.atom("UTF8_STRING" if target == "TEXT" else target), 8, PAYLOAD)
        # Stream a payload larger than both pipe buffers and X property chunks.
        peer.convert(MIME_BINARY)
        mime, writer = client.send_fd(source)
        assert mime == MIME_BINARY
        pid = os.fork()
        if pid == 0:
            try:
                view = memoryview(LARGE_PAYLOAD)
                while view:
                    view = view[os.write(writer, view):]
                os.close(writer)
                os._exit(0)
            except BaseException:
                os._exit(1)
        os.close(writer)
        assert peer.result() == (peer.atom(MIME_BINARY), 8, LARGE_PAYLOAD)
        assert os.waitpid(pid, 0)[1] == 0
        # A slow X11 reader cannot stop server round trips.
        peer.convert(MIME_BINARY)
        _, writer = client.send_fd(source)
        os.set_blocking(writer, False)
        try:
            while True:
                os.write(writer, b"x" * CHUNK_SIZE)
        except BlockingIOError:
            pass
        client.sync()
        os.close(writer)
        peer.result()
        # Foreign ownership cancels the Wayland source and imports TARGETS.
        peer.own()
        client.until(source, SOURCE_CANCELLED)
        request = peer.request("TARGETS")
        peer.answer(request, XA_ATOM, [peer.atom("UTF8_STRING"), peer.atom(MIME_BINARY)], 32)
        offer, events = client.selection()
        if not offer:
            offer, events = client.selection()
        assert {MIME_TEXT, MIME_BINARY, "text/plain"} <= set(offered(events, offer))
        for target, payload, expected in (("UTF8_STRING", PAYLOAD, PAYLOAD),
                                           (MIME_BINARY, bytes(range(256)), bytes(range(256)))):
            reader = receive_pipe(client, offer, MIME_TEXT if target == "UTF8_STRING" else target)
            request = peer.request(target)
            peer.answer(request, peer.atom(target), payload)
            assert pipe_result(reader) == expected
            os.close(reader)
        # INCR backpressure is driven by the Wayland reader, not server blocking.
        # Also accept owners that omit the advisory size and use large chunks.
        for size_hint in (True, False):
            reader = receive_pipe(client, offer, MIME_BINARY)
            request = peer.request(MIME_BINARY)
            pid = os.fork()
            if pid == 0:
                try:
                    assert pipe_result(reader) == LARGE_PAYLOAD
                    os._exit(0)
                except BaseException:
                    os._exit(1)
            try:
                peer.incr(request, peer.atom(MIME_BINARY), LARGE_PAYLOAD,
                          size_hint=size_hint,
                          chunk=CHUNK_SIZE if size_hint else LARGE_X_CHUNK)
                assert os.waitpid(pid, 0)[1] == 0
            finally:
                os.close(reader)
        # A reader that stops consuming keeps the X owner waiting for deletion.
        reader = receive_pipe(client, offer, MIME_BINARY)
        fcntl.fcntl(reader, fcntl.F_SETPIPE_SZ, BLOCKED_PIPE_SIZE)
        request = peer.request(MIME_BINARY)
        _, requestor, _, _, property = request
        peer.watch(requestor)
        peer.answer(request, peer.atom("INCR"), [len(LARGE_PAYLOAD)], 32)
        peer.event(PROPERTY_NOTIFY, lambda event:
                   struct.unpack_from("<II", event, 4) == (requestor, property)
                   and event[16] == PROPERTY_DELETE)
        peer.change(requestor, property, peer.atom(MIME_BINARY),
                    LARGE_PAYLOAD[:CHUNK_SIZE])
        client.sync()
        peer.sync()
        assert peer.get(requestor, property)[2] == LARGE_PAYLOAD[:CHUNK_SIZE]
        client.sync()
        os.close(reader)
        peer.event(17, lambda event: struct.unpack_from("<I", event, 8)[0] == requestor)
        peer.own(window=0)
        assert client.selection()[0] == 0
        # Legacy X text is converted from Latin-1 to UTF-8.
        peer.own()
        peer.answer(peer.request("TARGETS"), XA_ATOM, [XA_STRING], 32)
        offer, events = client.selection()
        if not offer:
            offer, events = client.selection()
        reader = receive_pipe(client, offer)
        peer.answer(peer.request(), XA_STRING, b"caf\xe9")
        assert pipe_result(reader) == "café".encode()
        os.close(reader)
        # Replacing an imported selection must leave the new Wayland source live.
        replacement = client.source()
        client.set_selection(replacement, serial)
        offer, _ = client.selection()
        assert offer
        client.sync()
        assert peer.owner() != peer.window
        client.send(replacement, SOURCE_DESTROY)
        assert client.selection()[0] == 0
        client.sync()
        assert peer.owner() == 0
        peer.own()
        peer.answer(peer.request("TARGETS"), XA_ATOM, [peer.atom("UTF8_STRING")], 32)
        offer, _ = client.selection()
        if not offer:
            offer, _ = client.selection()
        peer.close()
        peer = None
        assert client.selection()[0] == 0
    finally:
        if peer:
            peer.close()
        client.close()
        connection.close()


def main():
    host, module = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="meatball-selections-") as directory:
        path = Path(directory) / "wayland"
        with server(host, module, {"MEATBALL_TEST_SOCKET": str(path)},
                    arguments=["+extension", "XTEST"]) as (process, display):
            wait_socket(path, process)
            check_clipboard(path, display)
            # Reset while a foreign owner has an outstanding conversion.
            owner = XSelections(display)
            owner.own()
            owner.request("TARGETS")
            process.send_signal(signal.SIGHUP)
            while owner.connection.recv(32):
                pass
            owner.close()
            with connect_x11(display):
                wait_socket(path, process)
            check_clipboard(path, display)
            assert process.poll() is None


if __name__ == "__main__":
    main()
