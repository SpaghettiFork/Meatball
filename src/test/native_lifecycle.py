"""Check native module startup, reset, and failed socket initialization."""

from pathlib import Path
import signal
import socket
import stat
import struct
import sys
import tempfile
import time

from host_support import connect_x11, receive_exact, server, STARTUP_TIMEOUT

QUERY_EXTENSION = 98
X11_REPLY_SIZE = 32
X11_WORD_BYTES = 4
EXTENSION_NAME = b"MEATBALL"
SOCKET_MODE = 0o600
POLL_INTERVAL = 0.01
RESET_ATTEMPTS = 3


def check_extension(connection):
    padded = EXTENSION_NAME + bytes(-len(EXTENSION_NAME) % X11_WORD_BYTES)
    connection.sendall(struct.pack("<BBHHH", QUERY_EXTENSION, 0,
                                   (8 + len(padded)) // X11_WORD_BYTES,
                                   len(EXTENSION_NAME), 0) + padded)
    reply = receive_exact(connection, X11_REPLY_SIZE)
    assert reply[0] == 1 and reply[8], f"MEATBALL extension missing: {reply!r}"


def wait_socket(path, process):
    deadline = time.monotonic() + STARTUP_TIMEOUT
    while not path.is_socket():
        assert process.poll() is None, "X server exited during initialization"
        if time.monotonic() >= deadline:
            raise RuntimeError(f"Wayland socket did not appear: {path}")
        time.sleep(POLL_INTERVAL)
    assert stat.S_IMODE(path.stat().st_mode) == SOCKET_MODE


def connect_wayland(path):
    connection = socket.socket(socket.AF_UNIX)
    connection.settimeout(STARTUP_TIMEOUT)
    connection.connect(str(path))
    return connection


def check_lifecycle(host, module, path):
    environment = {"MEATBALL_TEST_SOCKET": str(path),
                   "MEATBALL_TEST_SOCKET_MODE": f"{SOCKET_MODE:o}"}
    with server(host, module, environment) as (process, display):
        connection = connect_x11(display)
        try:
            for _ in range(RESET_ATTEMPTS):
                check_extension(connection)
                wait_socket(path, process)
                with connect_wayland(path) as wayland:
                    process.send_signal(signal.SIGHUP)
                    assert connection.recv(1) == b"", "X client survived server reset"
                    connection.close()
                    try:
                        assert wayland.recv(1) == b"", "Wayland client survived reset"
                    except ConnectionResetError:
                        pass
                connection = connect_x11(display)
            check_extension(connection)
            wait_socket(path, process)
        finally:
            connection.close()
    assert not path.exists(), "Wayland socket survived server shutdown"
    assert not Path(f"{path}.lock").exists(), "Socket lock survived server shutdown"


def check_failed_start(host, module, path):
    with server(host, module, {"MEATBALL_TEST_SOCKET": str(path)}) as (_, display):
        with connect_x11(display) as connection:
            check_extension(connection)
        assert not path.exists()


def check_conflict(host, module, path):
    environment = {"MEATBALL_TEST_SOCKET": str(path)}
    with server(host, module, environment) as (first, display):
        with connect_x11(display) as connection:
            check_extension(connection)
        wait_socket(path, first)
        with server(host, module, environment) as (_, second_display):
            with connect_x11(second_display) as connection:
                check_extension(connection)
        with connect_wayland(path):
            pass
    assert not path.exists()


def main():
    host, module = sys.argv[1:3]
    with tempfile.TemporaryDirectory(prefix="meatball-native-") as directory:
        root = Path(directory)
        check_lifecycle(host, module, root / "wayland")
        check_failed_start(host, module, root / "missing" / "wayland")
        check_conflict(host, module, root / "wayland")


if __name__ == "__main__":
    main()
