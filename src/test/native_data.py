"""Exercise native Wayland clipboard ownership, serials, and FD transfers."""

import fcntl
import os
from pathlib import Path
import select
import signal
import struct
import sys
import tempfile

from host_support import connect_x11, server, STARTUP_TIMEOUT
from native_input import (InputClient, XInput, map_window, KEYBOARD_ENTER,
                          KEYBOARD_KEYMAP, KEYBOARD_LEAVE, KEY_PRESS, KEY_RELEASE,
                          KEY_A, KEYCODE_OFFSET, KEYBOARD_KEY)
from native_surfaces import root_window, query_tree
from native_xdg import string_argument
from native_lifecycle import wait_socket
from native_outputs import string
from wayland_support import Wayland, WaylandProtocolError

MANAGER_SOURCE = 0
MANAGER_DEVICE = 1
MANAGER_RELEASE = 2
SOURCE_OFFER = 0
SOURCE_DESTROY = 1
SOURCE_ACTIONS = 2
SOURCE_SEND = 1
SOURCE_CANCELLED = 2
DEVICE_DRAG = 0
DEVICE_SELECTION = 1
DEVICE_RELEASE = 2
DATA_OFFER = 0
SELECTION = 5
OFFER_MIME = 0
OFFER_RECEIVE = 1
OFFER_DESTROY = 2
OFFER_FINISH = 3
OFFER_ACTIONS = 4
MIME_TEXT = "text/plain;charset=utf-8"
MIME_BINARY = "application/octet-stream"
MIME_UNKNOWN = "application/x-meatball-unsupported"
PAYLOAD = "Meatball clipboard: zażółć\n".encode()
LARGE_BYTES = 1024 * 1024
COPY = 1
INVALID_ACTION = 1 << 8
PEER_ORIGIN = 96


class DataClient(InputClient):
    def __init__(self, path, version=None):
        self.data_ready = False
        super().__init__(path)
        self.data_ready = True
        self.manager = self.bind("wl_data_device_manager", version)
        self.manager_version = version or self.globals()["wl_data_device_manager"][1]
        self.device = self.allocate_id()
        self.send(self.manager, MANAGER_DEVICE, struct.pack("=II", self.device, self.seat))
        self.sync()

    def receive(self):
        event = super().receive()
        if self.data_ready and event[0] == self.keyboard and event[1] == KEYBOARD_KEYMAP:
            os.close(self.fds.pop(0))
        return event

    def source(self, mimes=(MIME_TEXT, MIME_BINARY)):
        source = self.allocate_id()
        self.send(self.manager, MANAGER_SOURCE, struct.pack("=I", source))
        for mime in mimes:
            self.send(source, SOURCE_OFFER, string_argument(mime))
        return source

    def set_selection(self, source, serial):
        self.send(self.device, DEVICE_SELECTION, struct.pack("=II", source, serial))

    def selection(self):
        data, events = self.until(self.device, SELECTION)
        offer, = struct.unpack("=I", data)
        if offer:
            assert (self.device, DATA_OFFER, struct.pack("=I", offer)) in events
        return offer, events

    def send_fd(self, source):
        data, _ = self.until(source, SOURCE_SEND)
        mime, offset = string(data)
        assert offset == len(data) and self.fds
        fd = self.fds.pop(0)
        assert fcntl.fcntl(fd, fcntl.F_GETFD) & fcntl.FD_CLOEXEC
        return mime, fd


def focus(client, actor, window):
    actor.focus(window)
    data, events = client.until(client.keyboard, KEYBOARD_ENTER)
    serial, = struct.unpack_from("=I", data)
    return serial, events


def mapped(path, actor, root, origin=None, version=None):
    before = query_tree(actor.connection, root)
    client = DataClient(path, version)
    window, = query_tree(actor.connection, root) - before
    map_window(client, actor.connection, root, window=window,
               **({"origin": origin} if origin is not None else {}))
    return client, window


def offered(events, offer):
    return [string(data)[0] for object_id, opcode, data in events
            if object_id == offer and opcode == OFFER_MIME]


def receive_pipe(client, offer, mime=MIME_TEXT):
    reader, writer = os.pipe2(os.O_CLOEXEC | os.O_NONBLOCK)
    try:
        client.send(offer, OFFER_RECEIVE, string_argument(mime), fds=[writer])
    finally:
        os.close(writer)
    return reader


def transfer(sender, source, receiver, offer, payload=PAYLOAD):
    reader = receive_pipe(receiver, offer)
    writer = None
    try:
        mime, writer = sender.send_fd(source)
        assert mime == MIME_TEXT
        sent, result = 0, bytearray()
        while sent < len(payload):
            try:
                sent += os.write(writer, payload[sent:])
            except BlockingIOError:
                pass
            while True:
                try:
                    result.extend(os.read(reader, LARGE_BYTES))
                except BlockingIOError:
                    break
        os.close(writer)
        writer = None
        while True:
            readable, _, _ = select.select([reader], [], [], STARTUP_TIMEOUT)
            assert readable, "transfer pipe remained open"
            data = os.read(reader, LARGE_BYTES)
            if not data:
                break
            result.extend(data)
        assert bytes(result) == payload
    finally:
        os.close(reader)
        if writer is not None:
            os.close(writer)


def rejected(receiver, offer, mime=MIME_TEXT):
    reader = receive_pipe(receiver, offer, mime)
    try:
        receiver.sync()
        readable, _, _ = select.select([reader], [], [], STARTUP_TIMEOUT)
        assert readable, "rejected transfer pipe remained open"
        assert os.read(reader, 1) == b""
    finally:
        os.close(reader)


def check_clipboard(path, display, process):
    x11, setup = connect_x11(display, return_setup=True)
    root = root_window(setup)
    actor = XInput(x11, root)
    sender = receiver = None
    try:
        sender, sender_window = mapped(path, actor, root)
        receiver, receiver_window = mapped(path, actor, root, origin=PEER_ORIGIN)
        serial, _ = focus(sender, actor, sender_window)
        source = sender.source((MIME_TEXT, MIME_BINARY, MIME_TEXT))
        sender.set_selection(source, serial)
        own_offer, events = sender.selection()
        assert offered(events, own_offer) == [MIME_TEXT, MIME_BINARY]
        peer_serial, events = focus(receiver, actor, receiver_window)
        offers = [struct.unpack("=I", data)[0] for object_id, opcode, data in events
                  if object_id == receiver.device and opcode == SELECTION]
        assert len(offers) == 1 and offers[0]
        peer_offer = offers[0]
        assert offered(events, peer_offer) == [MIME_TEXT, MIME_BINARY]
        transfer(sender, source, receiver, peer_offer)
        transfer(sender, source, receiver, peer_offer, payload=PAYLOAD * (LARGE_BYTES // len(PAYLOAD)))
        rejected(receiver, peer_offer, MIME_UNKNOWN)
        rejected(sender, own_offer)
        # A blocked transfer is owned by the two peers, leaving the server responsive.
        reader = receive_pipe(receiver, peer_offer)
        try:
            mime, writer = sender.send_fd(source)
            try:
                while True:
                    try:
                        os.write(writer, b"x" * LARGE_BYTES)
                    except BlockingIOError:
                        break
                receiver.sync()
                sender.sync()
            finally:
                os.close(writer)
        finally:
            os.close(reader)
        # A focused peer cannot use another peer's serial or an invented serial.
        replacement = receiver.source()
        receiver.set_selection(replacement, serial)
        receiver.set_selection(replacement, peer_serial + INVALID_ACTION)
        receiver.sync()
        transfer(sender, source, receiver, peer_offer)
        receiver.set_selection(replacement, peer_serial)
        new_offer, events = receiver.selection()
        sender.until(source, SOURCE_CANCELLED)
        rejected(receiver, peer_offer)
        transfer(receiver, replacement, receiver, new_offer)
        duplicate = receiver.allocate_id()
        receiver.send(receiver.manager, MANAGER_DEVICE, struct.pack("=II", duplicate, receiver.seat))
        data, events = receiver.until(duplicate, SELECTION)
        duplicate_offer, = struct.unpack("=I", data)
        assert duplicate_offer and offered(events, duplicate_offer) == [MIME_TEXT, MIME_BINARY]
        receiver.send(duplicate, DEVICE_RELEASE)
        receiver.sync()
        transfer(receiver, replacement, receiver, duplicate_offer)
        # Destroying a source clears the selection and invalidates existing offers.
        receiver.send(replacement, SOURCE_DESTROY)
        offer, _ = receiver.selection()
        assert offer == 0
        rejected(receiver, new_offer)
        receiver.set_selection(0, peer_serial)
        assert receiver.selection()[0] == 0
        if receiver.manager_version >= 4:
            receiver.send(receiver.manager, MANAGER_RELEASE)
            receiver.sync()
        assert process.poll() is None
    finally:
        if receiver:
            receiver.close()
        if sender:
            sender.close()
        x11.close()


def check_lifetimes(path, display, process):
    x11, setup = connect_x11(display, return_setup=True)
    root = root_window(setup)
    actor = XInput(x11, root)
    sender = receiver = None
    try:
        sender, sender_window = mapped(path, actor, root)
        receiver, receiver_window = mapped(path, actor, root, origin=PEER_ORIGIN)
        serial, _ = focus(sender, actor, sender_window)
        source = sender.source()
        sender.set_selection(source, serial)
        sender.selection()
        peer_serial, events = focus(receiver, actor, receiver_window)
        offer = [struct.unpack("=I", data)[0] for object_id, opcode, data in events
                 if object_id == receiver.device and opcode == SELECTION][-1]
        sender.close()
        sender = None
        assert receiver.selection()[0] == 0
        rejected(receiver, offer)
        source = receiver.source()
        actor.fake(KEY_PRESS, KEY_A + KEYCODE_OFFSET)
        data, _ = receiver.until(receiver.keyboard, KEYBOARD_KEY)
        key_serial, = struct.unpack_from("=I", data)
        actor.fake(KEY_RELEASE, KEY_A + KEYCODE_OFFSET)
        receiver.until(receiver.keyboard, KEYBOARD_KEY)
        receiver.set_selection(source, key_serial)
        offer, _ = receiver.selection()
        stale = receiver.source()
        receiver.set_selection(stale, peer_serial)
        receiver.sync()
        transfer(receiver, source, receiver, offer)
        process.send_signal(signal.SIGHUP)
        assert x11.recv(1) == b""
        receiver.disconnected()
    finally:
        if sender:
            sender.close()
        if receiver:
            receiver.close()
        x11.close()
    with connect_x11(display):
        wait_socket(path, process)
        client = DataClient(path)
        client.close()


def check_versions(path, display):
    probe = Wayland(path)
    maximum = probe.globals()["wl_data_device_manager"][1]
    probe.close()
    for version in range(1, maximum + 1):
        x11, setup = connect_x11(display, return_setup=True)
        root = root_window(setup)
        actor = XInput(x11, root)
        client, window = mapped(path, actor, root, version=version)
        try:
            serial, _ = focus(client, actor, window)
            source = client.source()
            client.set_selection(source, serial)
            offer, events = client.selection()
            assert offered(events, offer) == [MIME_TEXT, MIME_BINARY]
            transfer(client, source, client, offer)
            client.send(offer, OFFER_DESTROY)
            if version >= 2:
                client.send(client.device, DEVICE_RELEASE)
            client.sync()
        finally:
            client.close()
            x11.close()


def check_errors(path, display):
    for request, expected_object, code in (
            ("actions-selection", "source", 1),
            ("reuse", "device", 1),
            ("invalid-actions", "source", 0),
            ("finish-selection", "offer", 0),
            ("offer-actions", "offer", 3)):
        x11, setup = connect_x11(display, return_setup=True)
        actor = XInput(x11, root_window(setup))
        client, window = mapped(path, actor, root_window(setup))
        try:
            serial, _ = focus(client, actor, window)
            source = client.source()
            offer = 0
            if request == "actions-selection":
                client.send(source, SOURCE_ACTIONS, struct.pack("=I", COPY))
                client.set_selection(source, serial)
            elif request == "invalid-actions":
                client.send(source, SOURCE_ACTIONS, struct.pack("=I", INVALID_ACTION))
            else:
                client.set_selection(source, serial)
                offer, _ = client.selection()
                if request == "reuse":
                    client.set_selection(source, serial)
                elif request == "finish-selection":
                    client.send(offer, OFFER_FINISH)
                else:
                    client.send(offer, OFFER_ACTIONS, struct.pack("=II", COPY, COPY))
            try:
                client.sync()
                raise AssertionError(f"{request} accepted")
            except WaylandProtocolError as error:
                expected = {"source": source, "device": client.device, "offer": offer}
                assert error.object_id == expected[expected_object] and error.code == code
        finally:
            client.close()
            x11.close()


def main():
    host, module = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="meatball-data-") as directory:
        path = Path(directory) / "wayland"
        with server(host, module, {"MEATBALL_TEST_SOCKET": str(path)},
                    arguments=["+extension", "XTEST"]) as (process, display):
            wait_socket(path, process)
            check_clipboard(path, display, process)
            check_errors(path, display)
            check_versions(path, display)
            check_lifetimes(path, display, process)
            assert process.poll() is None


if __name__ == "__main__":
    main()
