"""Exercise a module in a real, isolated Spaghetti DIX/Xvfb server."""

from contextlib import contextmanager
import os
from pathlib import Path
import selectors
import socket
import struct
import subprocess
import sys
import tempfile


STARTUP_TIMEOUT = 10
SHUTDOWN_TIMEOUT = 5
X11_MAJOR = 11
ROOT_WIDTH = 128
ROOT_HEIGHT = 128
X11_SUCCESS = 1
SCREEN_DEPTH = 24
DISPLAY_RESPONSE_LIMIT = 64
X11_SETUP_PREFIX_SIZE = 8
X11_WORD_BYTES = 4


def receive_exact(connection, size):
    data = bytearray()
    while len(data) < size:
        chunk = connection.recv(size - len(data))
        if not chunk:
            raise RuntimeError("X server closed the connection")
        data.extend(chunk)
    return bytes(data)


def connect_x11(display, return_setup=False):
    connection = socket.socket(socket.AF_UNIX)
    connection.settimeout(STARTUP_TIMEOUT)
    try:
        connection.connect(f"/tmp/.X11-unix/X{display}")
        connection.sendall(struct.pack("<BBHHHHH", ord("l"), 0,
                                       X11_MAJOR, 0, 0, 0, 0))
        prefix = receive_exact(connection, X11_SETUP_PREFIX_SIZE)
        status, _, major, _, length = struct.unpack("<BBHHH", prefix)
        setup = receive_exact(connection, length * X11_WORD_BYTES)
        if status != X11_SUCCESS or major != X11_MAJOR:
            raise RuntimeError(f"X11 setup failed: {setup!r}")
        return (connection, setup) if return_setup else connection
    except BaseException:
        connection.close()
        raise


@contextmanager
def server(host, module, environment=None):
    read_fd, write_fd = os.pipe()
    child_environment = {**os.environ,
                         "MEATBALL_TEST_MODULE": str(Path(module).resolve())}
    for name, value in (environment or {}).items():
        if value is None:
            child_environment.pop(name, None)
        else:
            child_environment[name] = value
    with tempfile.TemporaryFile() as log:
        try:
            process = subprocess.Popen(
                [str(Path(host).resolve()), "-displayfd", str(write_fd),
                 "-nolisten", "tcp", "-ac", "-noreset", "-screen", "0",
                 f"{ROOT_WIDTH}x{ROOT_HEIGHT}x{SCREEN_DEPTH}"],
                env=child_environment,
                pass_fds=[write_fd], stdout=log, stderr=log,
            )
        finally:
            os.close(write_fd)
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(read_fd, selectors.EVENT_READ)
                if not selector.select(STARTUP_TIMEOUT):
                    raise RuntimeError("X server startup timed out")
                display = os.read(read_fd, DISPLAY_RESPONSE_LIMIT).strip().decode("ascii")
            if not display.isdecimal():
                raise RuntimeError("X server failed to report its display")
            yield process, display
        except BaseException:
            log.seek(0)
            sys.stderr.write(log.read().decode(errors="replace"))
            raise
        finally:
            os.close(read_fd)
            process.terminate()
            try:
                process.wait(SHUTDOWN_TIMEOUT)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                raise RuntimeError("X server shutdown timed out")
        if process.returncode:
            log.seek(0)
            sys.stderr.write(log.read().decode(errors="replace"))
            raise RuntimeError(f"X server exited with status {process.returncode}")
