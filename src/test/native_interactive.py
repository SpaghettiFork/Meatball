"""Validate drag serials and transfer native pointer grabs to a test WM."""

from pathlib import Path
import struct
import sys
import tempfile

from host_support import connect_x11, receive_exact, server
from native_clients import intern_atom
from native_input import (InputClient, XInput, map_window, MOTION, BUTTON_PRESS,
                          BUTTON_RELEASE, POINTER_BUTTON, WINDOW_ORIGIN,
                          X11_REPLY_SIZE, GET_INPUT_FOCUS)
from native_lifecycle import wait_socket
from native_surfaces import root_window
from native_xdg import (wm_message, CHANGE_ATTRIBUTES, CW_EVENT_MASK,
                        CLIENT_MESSAGE, SEND_EVENT_FLAG)

TOP_MOVE = 5
TOP_RESIZE = 6
GRAB_POINTER = 26
UNGRAB_POINTER = 27
GRAB_SUCCESS = 0
ASYNC = 1
SUBSTRUCTURE_NOTIFY = 1 << 19
BUTTON_RELEASE_MASK = 1 << 3
XDG_TO_EWMH_EDGES = ((5, 0), (1, 1), (9, 2), (8, 3), (10, 4), (2, 5), (6, 6), (4, 7))
WM_MOVE = 8
WM_SOURCE_APPLICATION = 1


def wm_barrier(wm):
    wm.sendall(struct.pack("<BBH", GET_INPUT_FOCUS, 0, 1))
    messages = []
    while True:
        event = receive_exact(wm, X11_REPLY_SIZE)
        assert event[0] != 0, event.hex()
        if event[0] == 1:
            return messages
        if event[0] & ~SEND_EVENT_FLAG == CLIENT_MESSAGE:
            messages.append(event)


def wm_grab(wm, root):
    wm.sendall(struct.pack("<BBHIHBBIII", GRAB_POINTER, 0, 6, root,
                           BUTTON_RELEASE_MASK, ASYNC, ASYNC, 0, 0, 0))
    while True:
        reply = receive_exact(wm, X11_REPLY_SIZE)
        assert reply[0] != 0, reply.hex()
        if reply[0] == 1:
            assert reply[1] == GRAB_SUCCESS, reply.hex()
            return


def check_interactive(path, actor, wm, root):
    client = InputClient(path)
    try:
        window = map_window(client, actor.connection, root)
        other_surface, other_xdg, other_top = client.add_toplevel()
        atom = intern_atom(actor.connection, "_NET_WM_MOVERESIZE")
        wm.sendall(struct.pack("<BBHIII", CHANGE_ATTRIBUTES, 0, 4,
                               root, CW_EVENT_MASK, SUBSTRUCTURE_NOTIFY))
        assert not wm_barrier(wm)
        for edge, direction in ((None, WM_MOVE),) + XDG_TO_EWMH_EDGES:
            actor.fake(MOTION, x=WINDOW_ORIGIN + 5, y=WINDOW_ORIGIN + 7)
            actor.fake(BUTTON_PRESS, 1)
            data, _ = client.until(client.pointer, POINTER_BUTTON)
            serial, _, button, state = struct.unpack("=IIII", data)
            assert state == 1
            payload = struct.pack("=II", client.seat, serial)
            opcode = TOP_MOVE if edge is None else TOP_RESIZE
            if edge is not None:
                payload += struct.pack("=I", edge)
            # Invalid serials and another toplevel cannot consume this grab.
            client.send(client.top, TOP_MOVE, struct.pack("=II", client.seat, serial - 1))
            client.send(other_top, TOP_MOVE, struct.pack("=II", client.seat, serial))
            client.sync()
            assert not wm_barrier(wm)
            client.send(client.top, opcode, payload)
            wm_message(wm, window, atom, (WINDOW_ORIGIN + 5, WINDOW_ORIGIN + 7,
                                          direction, 1, WM_SOURCE_APPLICATION))
            # The WM must be able to acquire the physical pointer for the drag.
            wm_grab(wm, root)
            client.send(client.top, opcode, payload)
            client.sync()
            assert not wm_barrier(wm)
            actor.fake(BUTTON_RELEASE, 1)
            actor.sync()
            wm.sendall(struct.pack("<BBHI", UNGRAB_POINTER, 0, 2, 0))
            assert not wm_barrier(wm)
            client.sync()
            client.send(client.top, opcode, payload)
            client.sync()
            assert not wm_barrier(wm)
    finally:
        client.close()


def main():
    host, module = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="meatball-interactive-") as directory:
        path = Path(directory) / "wayland"
        with server(host, module, {"MEATBALL_TEST_SOCKET": str(path)},
                    arguments=["+extension", "XTEST"]) as (process, display):
            wait_socket(path, process)
            x11, setup = connect_x11(display, return_setup=True)
            wm = connect_x11(display)
            try:
                root = root_window(setup)
                check_interactive(path, XInput(x11, root), wm, root)
                assert process.poll() is None
            finally:
                wm.close()
                x11.close()


if __name__ == "__main__":
    main()
