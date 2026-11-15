"""Check Wayland round trips alongside X11 traffic and output backpressure."""

from pathlib import Path
import socket
import struct
import sys
import tempfile

from host_support import connect_x11, server
from native_lifecycle import check_extension, wait_socket
from wayland_support import Wayland, WAYLAND_DISPLAY_ID, DISPLAY_SYNC

ROUND_TRIPS = 20
BLOCKED_CALLBACKS = 8192
RECEIVE_BUFFER_BYTES = 1024


def main():
    host, module = sys.argv[1:3]
    with tempfile.TemporaryDirectory(prefix="meatball-loop-") as directory:
        path = Path(directory) / "wayland"
        with server(host, module, {"MEATBALL_TEST_SOCKET": str(path)}) as (process, display):
            with connect_x11(display) as x11:
                check_extension(x11)
                wait_socket(path, process)
                client = Wayland(path)
                blocked = Wayland(path)
                try:
                    client.globals()
                    blocked.connection.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF,
                                                  RECEIVE_BUFFER_BYTES)
                    for _ in range(BLOCKED_CALLBACKS):
                        try:
                            blocked.send(WAYLAND_DISPLAY_ID, DISPLAY_SYNC,
                                         struct.pack("=I", blocked.allocate_id()))
                        except (BrokenPipeError, ConnectionResetError):
                            break
                    for _ in range(ROUND_TRIPS):
                        client.sync()
                        check_extension(x11)
                finally:
                    blocked.close()
                    client.close()


if __name__ == "__main__":
    main()
