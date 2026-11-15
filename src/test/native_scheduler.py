"""Exercise Wayland idle, timer, and FD sources in the Xorg scheduler."""

from pathlib import Path
import sys
import tempfile
import time

from host_support import connect_x11, server, STARTUP_TIMEOUT
from wayland_support import Wayland
from native_lifecycle import POLL_INTERVAL, wait_socket


def main():
    host, module = sys.argv[1:3]
    with tempfile.TemporaryDirectory(prefix="meatball-scheduler-") as directory:
        path, result = Path(directory) / "wayland", Path(directory) / "result"
        with server(host, module, {"MEATBALL_TEST_SOCKET": str(path),
                                   "MEATBALL_TEST_RESULT": str(result)}) as (process, display):
            with connect_x11(display):
                wait_socket(path, process)
                client = Wayland(path)
                try:
                    deadline = time.monotonic() + STARTUP_TIMEOUT
                    while not result.exists():
                        client.sync()
                        assert time.monotonic() < deadline, "Scheduler probe timed out"
                        time.sleep(POLL_INTERVAL)
                    assert result.read_bytes() == b"complete\0"
                finally:
                    client.close()


if __name__ == "__main__":
    main()
