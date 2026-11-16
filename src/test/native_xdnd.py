"""Exercise Xdnd messages and data transfers across native Wayland windows."""

from pathlib import Path
import os
import signal
import struct
import sys
import tempfile

from host_support import connect_x11, server
from native_data import (mapped, focus, receive_pipe, MIME_TEXT, MIME_BINARY,
                         PAYLOAD, SOURCE_CANCELLED)
from native_drag import (start, accept, ENTER, DROP, LEAVE, SOURCE_ACTION,
                         SOURCE_FINISHED, SOURCE_DROP, OFFER_FINISH,
                         OFFER_ACTIONS, COPY, MOVE, ASK, POINT_INSET,
                         SOURCE_POINT, TARGET_POINT, LEFT_BUTTON)
from native_input import XInput, MOTION, BUTTON_RELEASE, POINTER_ENTER
from native_lifecycle import wait_socket
from native_selections import (XSelections, XA_ATOM, pipe_result,
                                STRUCTURE_NOTIFY_MASK, PROPERTY_CHANGE_MASK)
from native_surfaces import root_window

CLIENT_MESSAGE = 33
XDND_VERSION = 5
VERSION_SHIFT = 24
POSITION_SHIFT = 16
ACCEPT = 1
WANT_POSITION = 2
TARGET_SIZE = 32
EXTRA_MIMES = ("text/uri-list", "application/x-meatball-xdnd")


class Xdnd(XSelections):
    def __init__(self, display):
        super().__init__(display)
        for name in ("XdndAware", "XdndProxy", "XdndTypeList", "XdndActionList",
                     "XdndEnter", "XdndPosition", "XdndStatus", "XdndLeave",
                     "XdndDrop", "XdndFinished", "XdndSelection",
                     "XdndActionCopy", "XdndActionMove", "XdndActionAsk"):
            self.atom(name)

    def target(self, x, y, version=XDND_VERSION):
        self.next_id += 1
        window = self.next_id
        self.send(1, 0, struct.pack("<IIhhHHHHIII", window, self.root,
                                   x, y, TARGET_SIZE, TARGET_SIZE, 0, 1, 0,
                                   1 << 11, STRUCTURE_NOTIFY_MASK | PROPERTY_CHANGE_MASK))
        self.change(window, self.atom("XdndAware"), XA_ATOM, [version], 32)
        self.send(8, 0, struct.pack("<I", window))
        self.sync()
        return window

    def send_message(self, destination, name, values, subject=None):
        assert len(values) <= 5
        values = [*values, *([0] * (5 - len(values)))]
        event = struct.pack("<BBHII5I", CLIENT_MESSAGE, 32, 0,
                            destination if subject is None else subject,
                            self.atom(name), *values)
        self.send(25, 0, struct.pack("<II", destination, 0) + event)

    def client_message(self, name):
        event = self.event(CLIENT_MESSAGE, lambda event:
                           struct.unpack_from("<I", event, 8)[0] == self.atom(name))
        subject, atom, *values = struct.unpack_from("<II5I", event, 4)
        return subject, values

    def status(self, source, target, action="XdndActionCopy", accepted=True):
        self.send_message(source, "XdndStatus", [target,
                          WANT_POSITION | (ACCEPT if accepted else 0), 0, 0,
                          self.atom(action) if accepted else 0])

    def position(self, target, x, y, action="XdndActionCopy"):
        self.send_message(target, "XdndPosition", [self.window, 0,
                          ((x & 0xffff) << POSITION_SHIFT) | (y & 0xffff),
                          0, self.atom(action)])


def source_action(client, source, expected):
    while True:
        payload, _ = client.until(source, SOURCE_ACTION)
        if struct.unpack("=I", payload)[0] == expected:
            return


def exported_transfer(peer, sender, source):
    peer.convert(MIME_BINARY, selection="XdndSelection")
    mime, writer = sender.send_fd(source)
    assert mime == MIME_BINARY
    os.write(writer, PAYLOAD)
    os.close(writer)
    assert peer.result("XdndSelection") == (peer.atom(MIME_BINARY), 8, PAYLOAD)


def check_export(path, display, version=XDND_VERSION, action=COPY, proxy=False,
                 queued=False, overlap=False):
    connection, setup = connect_x11(display, return_setup=True)
    root = root_window(setup)
    actor = XInput(connection, root)
    actor.fake(MOTION, x=0, y=0)
    actor.sync()
    sender, window = mapped(path, actor, root)
    peer = Xdnd(display)
    try:
        target = peer.target(TARGET_POINT - POINT_INSET,
                             SOURCE_POINT - POINT_INSET, version)
        recipient = target
        if proxy:
            recipient = peer.create_window()
            peer.change(recipient, peer.atom("XdndProxy"), 33, [recipient], 32)
            peer.change(recipient, peer.atom("XdndAware"), XA_ATOM, [version], 32)
            peer.change(target, peer.atom("XdndProxy"), 33, [recipient], 32)
            peer.sync()
        serial, _ = focus(sender, actor, window)
        clipboard = sender.source()
        sender.set_selection(clipboard, serial)
        sender.selection()
        sender.sync()
        clipboard_window = peer.owner()
        actor.fake(MOTION, x=SOURCE_POINT, y=SOURCE_POINT)
        sender.until(sender.pointer, POINTER_ENTER)
        source = sender.source((MIME_TEXT, MIME_BINARY, *EXTRA_MIMES))
        start(sender, actor, source=source)
        actor.fake(MOTION, x=TARGET_POINT, y=SOURCE_POINT)
        subject, enter = peer.client_message("XdndEnter")
        assert subject == target and enter[1] >> VERSION_SHIFT == version
        assert enter[1] & 1
        source_window = enter[0]
        assert peer.owner("XdndSelection") == source_window
        _, format, types = peer.get(source_window, peer.atom("XdndTypeList"))
        assert format == 32
        assert set(struct.unpack(f"<{len(types) // 4}I", types)) >= {
            peer.atom(mime) for mime in (MIME_TEXT, MIME_BINARY, *EXTRA_MIMES)}
        subject, position = peer.client_message("XdndPosition")
        assert subject == target and position[0] == source_window
        assert position[2] == TARGET_POINT << POSITION_SHIFT | SOURCE_POINT
        action_name = {COPY: "XdndActionCopy", MOVE: "XdndActionMove", ASK: "XdndActionAsk"}[action]
        if queued:
            actor.fake(MOTION, x=TARGET_POINT + 2, y=SOURCE_POINT)
            actor.fake(MOTION, x=TARGET_POINT + 3, y=SOURCE_POINT)
            actor.sync()
            sender.sync()
        peer.status(source_window, target, action_name)
        source_action(sender, source, action)
        exported_transfer(peer, sender, source)
        drop_seen = False
        if queued:
            _, cached = peer.client_message("XdndPosition")
            assert cached[2] == (TARGET_POINT + 3) << POSITION_SHIFT | SOURCE_POINT
            actor.fake(BUTTON_RELEASE, LEFT_BUTTON)
            actor.sync()
            events = sender.roundtrip_events()
            drop_seen = (source, SOURCE_DROP, b"") in events
            peer.sync()
            assert not any(event[0] & 0x7f == CLIENT_MESSAGE
                           and struct.unpack_from("<I", event, 8)[0] == peer.atom("XdndDrop")
                           for event in peer.events)
            peer.status(source_window, target, action_name)
        actor.fake(BUTTON_RELEASE, LEFT_BUTTON)
        subject, values = peer.client_message("XdndDrop")
        assert subject == target and values[0] == source_window
        if not drop_seen:
            sender.until(source, SOURCE_DROP)
        exported_transfer(peer, sender, source)
        if overlap:
            actor.fake(MOTION, x=SOURCE_POINT, y=SOURCE_POINT)
            sender.until(sender.pointer, POINTER_ENTER)
            next_source = start(sender, actor)
            actor.fake(MOTION, x=TARGET_POINT, y=SOURCE_POINT)
            actor.fake(BUTTON_RELEASE, LEFT_BUTTON)
            sender.until(next_source, SOURCE_CANCELLED)
            assert peer.owner("XdndSelection") == source_window
        peer.send_message(source_window, "XdndFinished", [target, ACCEPT,
                          peer.atom("XdndActionMove" if action == ASK else action_name)])
        if action == ASK:
            # A target chooses the final action when completing an ask drop.
            source_action(sender, source, MOVE)
            sender.until(source, SOURCE_FINISHED)
        else:
            sender.until(source, SOURCE_FINISHED)
        sender.sync()
        assert peer.owner("XdndSelection") == 0
        assert peer.owner() == clipboard_window
    finally:
        actor.fake(BUTTON_RELEASE, LEFT_BUTTON)
        actor.sync()
        sender.close()
        peer.close()
        connection.close()


def imported_transfer(peer, receiver, offer):
    reader = receive_pipe(receiver, offer, MIME_BINARY)
    try:
        request = peer.request(MIME_BINARY)
        peer.answer(request, peer.atom(MIME_BINARY), PAYLOAD)
        assert pipe_result(reader) == PAYLOAD
    finally:
        os.close(reader)


def incoming_offer(client):
    payload, _ = client.until(client.device, ENTER)
    serial, surface, x, y, offer = struct.unpack("=IIiiI", payload)
    assert surface == client.surface and offer
    assert (x, y) == (POINT_INSET * 256, POINT_INSET * 256)
    return serial, offer


def check_import(path, display, version=XDND_VERSION, action=COPY):
    connection, setup = connect_x11(display, return_setup=True)
    root = root_window(setup)
    actor = XInput(connection, root)
    receiver, window = mapped(path, actor, root)
    peer = Xdnd(display)
    try:
        assert peer.get(window, peer.atom("XdndAware")) == (
            XA_ATOM, 32, struct.pack("<I", XDND_VERSION))
        peer.own("XdndSelection")
        peer.change(peer.window, peer.atom("XdndActionList"), XA_ATOM,
                    [peer.atom("XdndActionCopy"), peer.atom("XdndActionMove"),
                     peer.atom("XdndActionAsk")], 32)
        peer.send_message(window, "XdndEnter", [peer.window, version << VERSION_SHIFT,
                          peer.atom("UTF8_STRING"), peer.atom(MIME_BINARY), 0])
        peer.position(window, SOURCE_POINT, SOURCE_POINT,
                      {COPY: "XdndActionCopy", MOVE: "XdndActionMove", ASK: "XdndActionAsk"}[action])
        serial, offer = incoming_offer(receiver)
        accept(receiver, offer, serial, action)
        while True:
            subject, status = peer.client_message("XdndStatus")
            assert subject == peer.window and status[0] == window
            if status[1] & ACCEPT:
                break
        assert status[4] == peer.atom({COPY: "XdndActionCopy", MOVE: "XdndActionMove",
                                      ASK: "XdndActionAsk"}[action])
        imported_transfer(peer, receiver, offer)
        peer.send_message(window, "XdndDrop", [peer.window, 0, 0])
        receiver.until(receiver.device, DROP)
        imported_transfer(peer, receiver, offer)
        if action == ASK:
            receiver.send(offer, OFFER_ACTIONS, struct.pack("=II", MOVE, MOVE))
        receiver.send(offer, OFFER_FINISH)
        subject, finished = peer.client_message("XdndFinished")
        assert subject == peer.window and finished[0] == window
        if version >= XDND_VERSION:
            assert finished[1] & ACCEPT
            assert finished[2] == peer.atom("XdndActionMove" if action in (MOVE, ASK)
                                           else "XdndActionCopy")
        else:
            assert finished[1:] == [0, 0, 0, 0]
        receiver.until(receiver.device, LEAVE)
        receiver.sync()
        assert peer.owner("XdndSelection") == peer.window
    finally:
        receiver.close()
        peer.close()
        connection.close()


def check_export_cancel(path, display, reason):
    connection, setup = connect_x11(display, return_setup=True)
    root = root_window(setup)
    actor = XInput(connection, root)
    actor.fake(MOTION, x=0, y=0)
    actor.sync()
    sender, window = mapped(path, actor, root)
    peer = Xdnd(display)
    try:
        target = peer.target(TARGET_POINT - POINT_INSET,
                             SOURCE_POINT - POINT_INSET)
        focus(sender, actor, window)
        actor.fake(MOTION, x=SOURCE_POINT, y=SOURCE_POINT)
        sender.until(sender.pointer, POINTER_ENTER)
        source = start(sender, actor)
        actor.fake(MOTION, x=TARGET_POINT, y=SOURCE_POINT)
        _, enter = peer.client_message("XdndEnter")
        peer.client_message("XdndPosition")
        source_window = enter[0]
        if reason in ("destroy", "disconnect"):
            peer.status(source_window, target)
            source_action(sender, source, COPY)
            actor.fake(BUTTON_RELEASE, LEFT_BUTTON)
            peer.client_message("XdndDrop")
            sender.until(source, SOURCE_DROP)
        if reason == "destroy":
            peer.send(4, 0, struct.pack("<I", target))
        elif reason == "disconnect":
            peer.close()
            peer = None
        elif reason == "reject":
            peer.status(source_window, target, accepted=False)
            actor.fake(BUTTON_RELEASE, LEFT_BUTTON)
        elif reason == "selection-loss":
            peer.own("XdndSelection")
        else:
            raise AssertionError(reason)
        sender.until(source, SOURCE_CANCELLED)
        sender.sync()
        if peer:
            expected = peer.window if reason == "selection-loss" else 0
            assert peer.owner("XdndSelection") == expected
    finally:
        actor.fake(BUTTON_RELEASE, LEFT_BUTTON)
        actor.sync()
        sender.close()
        if peer:
            peer.close()
        connection.close()


def check_import_cancel(path, display, reason):
    connection, setup = connect_x11(display, return_setup=True)
    root = root_window(setup)
    actor = XInput(connection, root)
    receiver, window = mapped(path, actor, root)
    peer = Xdnd(display)
    try:
        peer.own("XdndSelection")
        peer.send_message(window, "XdndEnter", [peer.window, XDND_VERSION << VERSION_SHIFT,
                          peer.atom("UTF8_STRING"), peer.atom(MIME_BINARY), 0])
        peer.position(window, SOURCE_POINT, SOURCE_POINT)
        serial, offer = incoming_offer(receiver)
        if reason != "reject":
            accept(receiver, offer, serial)
        if reason == "disconnect":
            peer.close()
            peer = None
        elif reason == "destroy":
            peer.send(4, 0, struct.pack("<I", peer.window))
        elif reason == "leave":
            peer.send_message(window, "XdndLeave", [peer.window])
        elif reason == "selection-loss":
            peer.own("XdndSelection", window=0)
        elif reason == "target":
            receiver.send(receiver.top, 0)
            receiver.send(receiver.xdg, 0)
            receiver.send(receiver.surface, 0)
        elif reason == "reject":
            peer.send_message(window, "XdndDrop", [peer.window, 0, 0])
            _, finished = peer.client_message("XdndFinished")
            assert finished[0] == window and not finished[1] & ACCEPT
        else:
            raise AssertionError(reason)
        receiver.until(receiver.device, LEAVE)
        reader = receive_pipe(receiver, offer)
        try:
            assert pipe_result(reader) == b""
        finally:
            os.close(reader)
        receiver.sync()
    finally:
        receiver.close()
        if peer:
            peer.close()
        connection.close()


def check_native_grab(path, display):
    connection, setup = connect_x11(display, return_setup=True)
    root = root_window(setup)
    actor = XInput(connection, root)
    receiver, window = mapped(path, actor, root)
    peer = Xdnd(display)
    observer = Xdnd(display)
    try:
        def grab(client):
            packet, _ = client.reply(client.send(26, 0, struct.pack(
                "<IHBBIII", root, 0, 1, 1, 0, 0, 0)))
            return packet[1]
        assert grab(peer) == 0
        peer.own("XdndSelection")
        peer.send_message(window, "XdndEnter", [peer.window, XDND_VERSION << VERSION_SHIFT,
                          peer.atom("UTF8_STRING"), peer.atom(MIME_BINARY), 0])
        peer.position(window, SOURCE_POINT, SOURCE_POINT)
        serial, offer = incoming_offer(receiver)
        accept(receiver, offer, serial)
        assert grab(observer) == 1, "incoming Xdnd replaced the X11 source's grab"
        peer.send_message(window, "XdndDrop", [peer.window, 0, 0])
        receiver.until(receiver.device, DROP)
        receiver.send(offer, OFFER_FINISH)
        peer.client_message("XdndFinished")
        assert grab(observer) == 1, "finishing Xdnd released the X11 source's grab"
        peer.send(27, 0, struct.pack("<I", 0))
        peer.sync()
        assert grab(observer) == 0
        observer.send(27, 0, struct.pack("<I", 0))
        observer.sync()
    finally:
        receiver.close()
        peer.close()
        observer.close()
        connection.close()


def check_reset(path, display, process):
    connection, setup = connect_x11(display, return_setup=True)
    root = root_window(setup)
    actor = XInput(connection, root)
    receiver, window = mapped(path, actor, root)
    peer = Xdnd(display)
    try:
        peer.own("XdndSelection")
        peer.send_message(window, "XdndEnter", [peer.window, XDND_VERSION << VERSION_SHIFT,
                          peer.atom("UTF8_STRING"), peer.atom(MIME_BINARY), 0])
        peer.position(window, SOURCE_POINT, SOURCE_POINT)
        serial, offer = incoming_offer(receiver)
        accept(receiver, offer, serial)
        reader = receive_pipe(receiver, offer)
        try:
            peer.request()
            process.send_signal(signal.SIGHUP)
            while connection.recv(32):
                pass
            receiver.disconnected()
            assert pipe_result(reader) == b""
        finally:
            os.close(reader)
    finally:
        receiver.close()
        peer.close()
        connection.close()
    with connect_x11(display):
        wait_socket(path, process)
    check_export(path, display)
    check_import(path, display)


def main():
    host, module = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="meatball-xdnd-") as directory:
        path = Path(directory) / "wayland"
        with server(host, module, {"MEATBALL_TEST_SOCKET": str(path)},
                    arguments=["+extension", "XTEST"]) as (process, display):
            wait_socket(path, process)
            for version in range(3, XDND_VERSION + 1):
                check_export(path, display, version)
                check_import(path, display, version)
            check_export(path, display, action=MOVE)
            check_export(path, display, action=ASK)
            check_export(path, display, proxy=True)
            check_export(path, display, queued=True)
            check_export(path, display, overlap=True)
            check_import(path, display, action=MOVE)
            check_import(path, display, action=ASK)
            for reason in ("destroy", "disconnect", "reject", "selection-loss"):
                check_export_cancel(path, display, reason)
            for reason in ("destroy", "disconnect", "leave", "target", "reject", "selection-loss"):
                check_import_cancel(path, display, reason)
            check_native_grab(path, display)
            check_reset(path, display, process)
            assert process.poll() is None


if __name__ == "__main__":
    main()
