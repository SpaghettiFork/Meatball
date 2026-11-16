"""Exercise native drag grabs, action negotiation, transfers, and cleanup."""

from contextlib import contextmanager
from pathlib import Path
import signal
import struct
import sys
import tempfile

from host_support import connect_x11, receive_exact, server
from native_data import (DataClient, mapped, focus, transfer, rejected, offered,
                         DEVICE_DRAG, DEVICE_RELEASE, SOURCE_ACTIONS,
                         SOURCE_DESTROY, SOURCE_CANCELLED, OFFER_DESTROY,
                         OFFER_FINISH, OFFER_ACTIONS, MIME_TEXT, MIME_BINARY, COPY,
                         MANAGER_DEVICE, PEER_ORIGIN)
from native_input import (XInput, MOTION, BUTTON_PRESS, BUTTON_RELEASE,
                          POINTER_BUTTON, POINTER_LEAVE, POINTER_ENTER,
                          KEY_PRESS, KEY_RELEASE, KEYCODE_OFFSET, WINDOW_ORIGIN,
                          extension, X11_REPLY_SIZE)
from native_lifecycle import wait_socket
from native_surfaces import (root_window, query_tree, create_surface,
                             shape_rectangles, SHAPE_BOUNDING, WL_SURFACE_FRAME,
                             WL_SURFACE_COMMIT, WL_SURFACE_DESTROY)
from native_subsurfaces import publish, solid, BLUE, child
from native_transforms import GET_GEOMETRY
from native_cursor import (cursor_image, XFIXES_QUERY_VERSION, SET_CURSOR, PIXELS)
from native_xdg import (map_state, VIEWABLE, UNMAPPED, string_argument,
                        TOP_DESTROY, XDG_DESTROY)
from wayland_support import WaylandProtocolError

ENTER = 1
LEAVE = 2
DRAG_MOTION = 3
DROP = 4
SOURCE_DROP = 3
SOURCE_FINISHED = 4
SOURCE_ACTION = 5
OFFER_ACTION = 2
OFFER_ACCEPT = 0
POINT_INSET = 8
SOURCE_POINT = WINDOW_ORIGIN + POINT_INSET
TARGET_POINT = PEER_ORIGIN + POINT_INSET
MOVE = 2
ASK = 4
ACTIONS = COPY | MOVE | ASK
LEFT_BUTTON = 1
KEY_ESC = 1
FIXED_ONE = 256
INVALID_SERIAL_OFFSET = 0x10000
ACTIONS_VERSION = 3
OFFER_INVALID_FINISH = 0
OFFER_INVALID_ACTION_MASK = 1
OFFER_INVALID_ACTION = 2
OFFER_INVALID_OFFER = 3
DATA_ROLE_ERROR = 0


@contextmanager
def peers(path, display, source_version=None, target_version=None):
    x11, setup = connect_x11(display, return_setup=True)
    root = root_window(setup)
    actor = XInput(x11, root)
    sender = receiver = None
    actor.fake(MOTION, x=0, y=0)
    actor.sync()
    try:
        sender, source_window = mapped(path, actor, root, version=source_version)
        receiver, _ = mapped(path, actor, root, origin=PEER_ORIGIN, version=target_version)
        focus(sender, actor, source_window)
        actor.fake(MOTION, x=SOURCE_POINT, y=SOURCE_POINT)
        payload, _ = sender.until(sender.pointer, POINTER_ENTER)
        sender.pointer_serial = struct.unpack_from("=I", payload)[0]
        yield sender, receiver, actor
    finally:
        actor.fake(BUTTON_RELEASE, LEFT_BUTTON)
        actor.fake(KEY_RELEASE, KEY_ESC + KEYCODE_OFFSET)
        actor.sync()
        if sender:
            sender.close()
        if receiver:
            receiver.close()
        x11.close()


def press(sender, actor):
    actor.fake(BUTTON_PRESS, LEFT_BUTTON)
    payload, _ = sender.until(sender.pointer, POINTER_BUTTON)
    serial, _, _, state = struct.unpack("=IIII", payload)
    assert state == 1
    return serial


def start(sender, actor, actions=ACTIONS, source=None, icon=0):
    if source is None:
        source = sender.source()
    if source and sender.manager_version >= ACTIONS_VERSION:
        sender.send(source, SOURCE_ACTIONS, struct.pack("=I", actions))
    serial = press(sender, actor)
    sender.send(sender.device, DEVICE_DRAG,
                struct.pack("=IIII", source, sender.surface, icon, serial))
    sender.until(sender.pointer, POINTER_LEAVE)
    payload, _ = sender.until(sender.device, ENTER)
    assert struct.unpack_from("=I", payload, 4)[0] == sender.surface
    return source


def enter(receiver, actor):
    actor.fake(MOTION, x=TARGET_POINT, y=SOURCE_POINT)
    payload, events = receiver.until(receiver.device, ENTER)
    serial, surface, x, y, offer = struct.unpack("=IIiiI", payload)
    assert surface == receiver.surface
    assert (x, y) == (POINT_INSET * FIXED_ONE, POINT_INSET * FIXED_ONE), (x, y)
    if offer:
        assert offered(events, offer) == [MIME_TEXT, MIME_BINARY]
    return serial, offer


def accept(receiver, offer, serial, action=COPY):
    receiver.send(offer, OFFER_ACCEPT, struct.pack("=I", serial) + string_argument(MIME_TEXT))
    if receiver.manager_version >= ACTIONS_VERSION:
        receiver.send(offer, OFFER_ACTIONS, struct.pack("=II", ACTIONS, action))
        while True:
            payload, _ = receiver.until(offer, OFFER_ACTION)
            if struct.unpack("=I", payload)[0] == action:
                break
    receiver.sync()


def drop(sender, source, receiver, actor):
    actor.fake(BUTTON_RELEASE, LEFT_BUTTON)
    receiver.until(receiver.device, DROP)
    if source and sender.manager_version >= ACTIONS_VERSION:
        sender.until(source, SOURCE_DROP)


def check_transfer(path, display, source_version=None, target_version=None, action=COPY):
    with peers(path, display, source_version, target_version) as (sender, receiver, actor):
        source = start(sender, actor)
        serial, offer = enter(receiver, actor)
        accept(receiver, offer, serial, action)
        transfer(sender, source, receiver, offer)
        actor.fake(MOTION, x=TARGET_POINT + 2, y=SOURCE_POINT + 3)
        payload, _ = receiver.until(receiver.device, DRAG_MOTION)
        assert struct.unpack("=Iii", payload)[1:] == (10 * FIXED_ONE, 11 * FIXED_ONE)
        drop(sender, source, receiver, actor)
        transfer(sender, source, receiver, offer)
        if receiver.manager_version >= ACTIONS_VERSION:
            if action == ASK:
                receiver.send(offer, OFFER_ACTIONS, struct.pack("=II", MOVE, MOVE))
                receiver.sync()
            receiver.send(offer, OFFER_FINISH)
        else:
            receiver.send(offer, OFFER_DESTROY)
        if sender.manager_version >= ACTIONS_VERSION:
            _, events = sender.until(source, SOURCE_FINISHED)
            if action == ASK:
                assert events[-2] == (source, SOURCE_ACTION, struct.pack("=I", MOVE)), events
        receiver.until(receiver.device, LEAVE)
        actor.fake(MOTION, x=SOURCE_POINT, y=SOURCE_POINT)
        sender.until(sender.pointer, POINTER_ENTER)
        if receiver.manager_version >= ACTIONS_VERSION:
            receiver.send(offer, OFFER_DESTROY)
        sender.send(source, SOURCE_DESTROY)
        sender.sync()
        receiver.sync()


def check_serials(path, display):
    with peers(path, display) as (sender, receiver, actor):
        source = sender.source()
        serial = press(sender, actor)
        sender.send(sender.device, DEVICE_DRAG, struct.pack("=IIII", source, sender.surface, 0,
                                                         serial + INVALID_SERIAL_OFFSET))
        events = sender.roundtrip_events()
        assert not any(obj == sender.device and op == ENTER for obj, op, _ in events)
        sender.send(sender.device, DEVICE_DRAG, struct.pack("=IIII", source, sender.surface, 0, serial))
        sender.until(sender.pointer, POINTER_LEAVE)
        sender.until(sender.device, ENTER)
        actor.fake(BUTTON_RELEASE, LEFT_BUTTON)
        sender.until(source, SOURCE_CANCELLED)  # No actions or accepted MIME.
        sender.send(source, SOURCE_DESTROY)


def check_cancel(path, display):
    for reason in ("escape", "source", "target", "device", "reject", "outside", "offer"):
        with peers(path, display) as (sender, receiver, actor):
            source = start(sender, actor)
            serial, offer = enter(receiver, actor)
            accept(receiver, offer, serial)
            if reason == "escape":
                actor.fake(KEY_PRESS, KEY_ESC + KEYCODE_OFFSET)
            elif reason == "source":
                sender.send(source, SOURCE_DESTROY)
            elif reason == "target":
                receiver.send(receiver.top, TOP_DESTROY)
                receiver.send(receiver.xdg, XDG_DESTROY)
                receiver.send(receiver.surface, WL_SURFACE_DESTROY)
                receiver.sync()
                actor.fake(BUTTON_RELEASE, LEFT_BUTTON)
            elif reason == "device":
                receiver.send(receiver.device, DEVICE_RELEASE)
            elif reason == "reject":
                receiver.send(offer, OFFER_ACCEPT, struct.pack("=II", serial, 0))
                receiver.sync()
                actor.fake(BUTTON_RELEASE, LEFT_BUTTON)
            elif reason == "outside":
                actor.fake(MOTION, x=0, y=0)
                receiver.until(receiver.device, LEAVE)
                actor.fake(BUTTON_RELEASE, LEFT_BUTTON)
            elif reason == "offer":
                drop(sender, source, receiver, actor)
                receiver.send(offer, OFFER_DESTROY)
            if reason != "source":
                sender.until(source, SOURCE_CANCELLED)
            else:
                receiver.until(receiver.device, LEAVE)
            if reason != "offer":
                rejected(receiver, offer)
            sender.sync()
            receiver.sync()


def check_internal(path, display):
    with peers(path, display) as (sender, receiver, actor):
        start(sender, actor, source=0)
        actor.fake(MOTION, x=TARGET_POINT, y=SOURCE_POINT)
        sender.until(sender.device, LEAVE)
        receiver.sync()
        events = receiver.roundtrip_events()
        assert not any(obj == receiver.device and op == ENTER for obj, op, _ in events)
        actor.fake(MOTION, x=SOURCE_POINT, y=SOURCE_POINT)
        payload, _ = sender.until(sender.device, ENTER)
        assert struct.unpack_from("=I", payload, 16)[0] == 0
        drop(sender, 0, sender, actor)
        sender.until(sender.device, LEAVE)


def check_icon(path, display):
    with peers(path, display) as (sender, receiver, actor):
        x11, root = actor.connection, actor.root
        before = query_tree(x11, root)
        icon = create_surface(sender, sender.compositor)
        window, = query_tree(x11, root) - before
        callback = sender.allocate_id()
        sender.send(icon, WL_SURFACE_FRAME, struct.pack("=I", callback))
        publish(sender, icon, PIXELS)
        assert map_state(x11, window) == UNMAPPED
        source = start(sender, actor, icon=icon)
        sender.until(callback, 0)
        assert map_state(x11, window) == VIEWABLE
        assert shape_rectangles(x11, window) == []
        assert shape_rectangles(x11, window, SHAPE_BOUNDING) == [(0, 0, 2, 1), (1, 1, 1, 1)]
        cursor = create_surface(sender, sender.compositor)
        fixes = extension(x11, "XFIXES")
        x11.sendall(struct.pack("<BBHII", fixes, XFIXES_QUERY_VERSION, 3, 5, 0))
        assert receive_exact(x11, X11_REPLY_SIZE)[0] == 1
        sender.send(sender.pointer, SET_CURSOR,
                    struct.pack("=IIii", sender.pointer_serial, cursor, 1, 1))
        publish(sender, cursor, PIXELS)
        assert cursor_image(x11, fixes) == ((2, 2, 1, 1), sum(PIXELS, ()))
        serial, offer = enter(receiver, actor)
        accept(receiver, offer, serial)
        sender.sync()
        x11.sendall(struct.pack("<BBHI", GET_GEOMETRY, 0, 2, window))
        reply = receive_exact(x11, X11_REPLY_SIZE)
        assert reply[0] == 1
        assert struct.unpack_from("<hh", reply, 12) == (TARGET_POINT, SOURCE_POINT)
        nested, role = child(sender, icon, -1, -1)
        publish(sender, nested, solid(BLUE, 1, 1))
        sender.send(icon, WL_SURFACE_COMMIT)
        sender.sync()
        assert shape_rectangles(x11, window) == []
        drop(sender, source, receiver, actor)
        assert map_state(x11, window) == UNMAPPED
        # Reusing an icon during an older offer's transfer must retain its mapping.
        actor.fake(MOTION, x=SOURCE_POINT, y=SOURCE_POINT)
        payload, _ = sender.until(sender.pointer, POINTER_ENTER)
        sender.pointer_serial = struct.unpack_from("=I", payload)[0]
        second = start(sender, actor, icon=icon)
        assert map_state(x11, window) == VIEWABLE
        receiver.send(offer, OFFER_FINISH)
        sender.until(source, SOURCE_FINISHED)
        assert map_state(x11, window) == VIEWABLE
        actor.fake(KEY_PRESS, KEY_ESC + KEYCODE_OFFSET)
        sender.until(second, SOURCE_CANCELLED)
        assert map_state(x11, window) == UNMAPPED


def check_errors(path, display):
    for reason, code in (("finish-early", OFFER_INVALID_FINISH),
                         ("mask", OFFER_INVALID_ACTION_MASK),
                         ("preferred", OFFER_INVALID_ACTION),
                         ("finish-twice", OFFER_INVALID_FINISH),
                         ("after-finish", OFFER_INVALID_OFFER),
                         ("ask-action", OFFER_INVALID_ACTION)):
        with peers(path, display) as (sender, receiver, actor):
            source = start(sender, actor, actions=COPY | ASK if reason == "ask-action" else ACTIONS)
            serial, offer = enter(receiver, actor)
            accept(receiver, offer, serial, ASK if reason == "ask-action" else COPY)
            if reason in ("finish-twice", "after-finish", "ask-action"):
                drop(sender, source, receiver, actor)
                if reason != "ask-action":
                    receiver.send(offer, OFFER_FINISH)
                    sender.until(source, SOURCE_FINISHED)
            if reason in ("finish-early", "finish-twice"):
                receiver.send(offer, OFFER_FINISH)
            elif reason == "mask":
                receiver.send(offer, OFFER_ACTIONS, struct.pack("=II", INVALID_SERIAL_OFFSET, COPY))
            elif reason == "preferred":
                receiver.send(offer, OFFER_ACTIONS, struct.pack("=II", COPY, MOVE))
            elif reason == "after-finish":
                receiver.send(offer, OFFER_ACCEPT, struct.pack("=II", serial, 0))
            elif reason == "ask-action":
                receiver.send(offer, OFFER_ACTIONS, struct.pack("=II", MOVE, MOVE))
            try:
                receiver.sync()
                raise AssertionError(f"{reason} accepted")
            except WaylandProtocolError as error:
                assert error.object_id == offer and error.code == code, (reason, error.code)


def check_roles(path, display):
    with peers(path, display) as (sender, receiver, actor):
        source = sender.source()
        serial = press(sender, actor)
        sender.send(sender.device, DEVICE_DRAG,
                    struct.pack("=IIII", source, sender.surface, sender.surface, serial))
        try:
            sender.sync()
            raise AssertionError("toplevel accepted a drag icon role")
        except WaylandProtocolError as error:
            assert error.object_id == sender.device and error.code == DATA_ROLE_ERROR


def check_bindings(path, display):
    with peers(path, display) as (sender, receiver, actor):
        duplicate = receiver.allocate_id()
        receiver.send(receiver.manager, MANAGER_DEVICE, struct.pack("=II", duplicate, receiver.seat))
        receiver.sync()
        source = start(sender, actor)
        serial, offer = enter(receiver, actor)
        payload, _ = receiver.until(duplicate, ENTER)
        extra_serial, _, _, _, extra_offer = struct.unpack("=IIiiI", payload)
        assert extra_serial == serial and extra_offer != offer
        accept(receiver, offer, serial)
        accept(receiver, extra_offer, serial, MOVE)
        actor.fake(BUTTON_RELEASE, LEFT_BUTTON)
        payload, events = receiver.until(duplicate, DROP)
        assert (receiver.device, LEAVE, b'') in events
        sender.until(source, SOURCE_DROP)
        rejected(receiver, offer)
        transfer(sender, source, receiver, extra_offer)
        receiver.send(extra_offer, OFFER_FINISH)
        sender.until(source, SOURCE_FINISHED)
        receiver.until(duplicate, LEAVE)


def check_disconnect(path, display):
    for dropped in (False, True):
        for side in ("source", "target"):
            with peers(path, display) as (sender, receiver, actor):
                source = start(sender, actor)
                serial, offer = enter(receiver, actor)
                accept(receiver, offer, serial)
                if dropped:
                    drop(sender, source, receiver, actor)
                if side == "target":
                    receiver.close()
                    if not dropped:
                        actor.fake(BUTTON_RELEASE, LEFT_BUTTON)
                    sender.until(source, SOURCE_CANCELLED)
                    sender.sync()
                else:
                    sender.close()
                    receiver.until(receiver.device, LEAVE)
                    rejected(receiver, offer)
                    receiver.sync()


def check_reset(path, display, process):
    for dropped in (False, True):
        # Reset closes the input injector too, so cleanup runs after the reset.
        x11, setup = connect_x11(display, return_setup=True)
        root = root_window(setup)
        actor = XInput(x11, root)
        actor.fake(MOTION, x=0, y=0)
        actor.sync()
        sender, window = mapped(path, actor, root)
        receiver, _ = mapped(path, actor, root, origin=PEER_ORIGIN)
        try:
            focus(sender, actor, window)
            actor.fake(MOTION, x=SOURCE_POINT, y=SOURCE_POINT)
            sender.until(sender.pointer, POINTER_ENTER)
            source = start(sender, actor)
            serial, offer = enter(receiver, actor)
            accept(receiver, offer, serial)
            if dropped:
                drop(sender, source, receiver, actor)
            process.send_signal(signal.SIGHUP)
            assert x11.recv(1) == b''
            sender.disconnected()
            receiver.disconnected()
        finally:
            sender.close()
            receiver.close()
            x11.close()
        with connect_x11(display):
            wait_socket(path, process)
            client = DataClient(path)
            client.close()


def main():
    host, module = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="meatball-drag-") as directory:
        path = Path(directory) / "wayland"
        with server(host, module, {"MEATBALL_TEST_SOCKET": str(path)},
                    arguments=["+extension", "XTEST"]) as (process, display):
            wait_socket(path, process)
            check_transfer(path, display)
            check_transfer(path, display, action=MOVE)
            check_transfer(path, display, action=ASK)
            for source_version, target_version in ((1, ACTIONS_VERSION), (2, ACTIONS_VERSION),
                                                   (ACTIONS_VERSION, 1), (ACTIONS_VERSION, 2)):
                check_transfer(path, display, source_version, target_version)
            check_serials(path, display)
            check_cancel(path, display)
            check_internal(path, display)
            check_icon(path, display)
            check_errors(path, display)
            check_roles(path, display)
            check_bindings(path, display)
            check_disconnect(path, display)
            check_reset(path, display, process)
            assert process.poll() is None


if __name__ == "__main__":
    main()
