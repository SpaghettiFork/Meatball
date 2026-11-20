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
from wayland_support import Wayland

QUERY_EXTENSION = 98
X11_REPLY_SIZE = 32
X11_WORD_BYTES = 4
EXTENSION_NAME = b"MEATBALL"
SOCKET_MODE = 0o600
POLL_INTERVAL = 0.01
RESET_ATTEMPTS = 3
CUSTOM_SOCKET_MODE = 0o640
SOCKET_PREFIX = "wayland-meatball-"
KILL_CLIENT = 113


def check_extension(connection):
    padded = EXTENSION_NAME + bytes(-len(EXTENSION_NAME) % X11_WORD_BYTES)
    connection.sendall(struct.pack("<BBHHH", QUERY_EXTENSION, 0,
                                   (8 + len(padded)) // X11_WORD_BYTES,
                                   len(EXTENSION_NAME), 0) + padded)
    reply = receive_exact(connection, X11_REPLY_SIZE)
    assert reply[0] == 1 and reply[8], f"MEATBALL extension missing: {reply!r}"


def wait_socket(path, process):
    deadline = time.monotonic() + STARTUP_TIMEOUT
    while True:
        try:
            info = path.stat()
        except FileNotFoundError:
            pass
        else:
            if stat.S_ISSOCK(info.st_mode) and stat.S_IMODE(info.st_mode) == SOCKET_MODE:
                return
        assert process.poll() is None, "X server exited during initialization"
        if time.monotonic() >= deadline:
            raise RuntimeError(f"Wayland socket did not become ready with mode {SOCKET_MODE:o}: {path}")
        time.sleep(POLL_INTERVAL)


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


def check_runtime_directory(host, module, directory):
    path = directory / "wayland"
    with server(host, module, {"MEATBALL_TEST_SOCKET": str(path)}) as (process, display):
        with connect_x11(display) as connection:
            check_extension(connection)
            assert not path.exists()
            directory.mkdir()
            wait_socket(path, process)
            with connect_wayland(path) as old:
                path.unlink()
                path.with_suffix(".lock").unlink()
                directory.rmdir()
                try:
                    assert old.recv(1) == b"", "Wayland client survived socket removal"
                except ConnectionResetError:
                    pass
            directory.mkdir()
            wait_socket(path, process)
            client = Wayland(path)
            try:
                client.sync()
            finally:
                client.close()
            check_extension(connection)
    assert not path.exists()


def check_bridge_closed(host, module, path):
    with server(host, module, {"MEATBALL_TEST_SOCKET": str(path)}) as (process, display):
        connection, setup = connect_x11(display, return_setup=True)
        # The isolated server has one root and only the bridge's selection
        # window at this point.
        from native_surfaces import query_tree, root_window
        with connection:
            wait_socket(path, process)
            window, = query_tree(connection, root_window(setup))
            with connect_wayland(path) as old:
                connection.sendall(struct.pack("<BBHI", KILL_CLIENT, 0, 2, window))
                try:
                    assert old.recv(1) == b"", "Wayland client survived bridge loss"
                except ConnectionResetError:
                    pass
            wait_socket(path, process)
            client = Wayland(path)
            try:
                client.sync()
            finally:
                client.close()
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


def check_configuration(host, module, directory):
    path = directory / "custom"
    with server(host, module, {"MEATBALL_TEST_SOCKET": str(path),
                               "MEATBALL_TEST_SOCKET_MODE": f"{CUSTOM_SOCKET_MODE:o}"}) as (_, display):
        with connect_x11(display) as connection:
            check_extension(connection)
        assert path.is_socket()
        assert stat.S_IMODE(path.stat().st_mode) == CUSTOM_SOCKET_MODE
    with server(host, module, {"MEATBALL_TEST_SOCKET": None,
                               "MEATBALL_TEST_SOCKET_MODE": None,
                               "XDG_RUNTIME_DIR": str(directory)}) as (process, display):
        with connect_x11(display) as connection:
            check_extension(connection)
        path = directory / f"{SOCKET_PREFIX}{display}"
        wait_socket(path, process)
    assert not path.exists()


def check_capabilities(host, module, directory):
    for extension in ["MIT-SHM", "RENDER"]:
        path = directory / "disabled"
        with server(host, module, {"MEATBALL_TEST_SOCKET": str(path)},
                    arguments=["-extension", extension]) as (_, display):
            with connect_x11(display) as connection:
                check_extension(connection)
            assert not path.exists(), f"Meatball started without {extension}"


def main():
    host, module = sys.argv[1:3]
    with tempfile.TemporaryDirectory(prefix="meatball-native-") as directory:
        root = Path(directory)
        check_lifecycle(host, module, root / "wayland")
        check_failed_start(host, module, root / "missing" / "wayland")
        check_runtime_directory(host, module, root / "runtime")
        check_bridge_closed(host, module, root / "bridge")
        check_conflict(host, module, root / "wayland")
        check_configuration(host, module, root)
        check_capabilities(host, module, root)


if __name__ == "__main__":
    main()
