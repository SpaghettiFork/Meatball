"""Exercise native resource ownership and deferred Xorg output delivery."""

from pathlib import Path
import signal
import struct
import sys
import tempfile

from host_support import connect_x11, receive_exact, server
from native_lifecycle import wait_socket
from wayland_support import Wayland

TEST_INTERFACE = "meatball_client_test"
OWNERSHIP = 1
OUTPUT = 2
OVERSIZED = 3
OVERFLOW = 4
EXCEPTION = 5
ORPHAN_FD = 6
LOOKUP = 7
FD_CLOSED = 8
NATIVE_CLOSE = 9
CLOSE_IN_OUTPUT = 10
FD_OVERFLOW = 11
RESULT_OWNERSHIP = 1
RESULT_OUTPUT = 2
RESULT_LOOKUP = 3
RESULT_FD_CLOSED = 4
OUTPUT_STAGES = 5
KILL_CLIENT = 113
GET_GEOMETRY = 14
X11_REPLY_SIZE = 32
BAD_DRAWABLE = 9
U32_BITS = 32


class TestClient(Wayland):
    def __init__(self, path):
        super().__init__(path)
        self.test = self.bind(TEST_INTERFACE)

    def exercise(self, operation, high=0, low=0):
        self.send(self.test, 0, struct.pack("=III", operation, high, low))

    def result(self):
        while True:
            object_id, opcode, payload = self.receive()
            if object_id == self.test:
                assert opcode == 0
                return struct.unpack("=IIII", payload)

    def ownership(self):
        self.exercise(OWNERSHIP)
        kind, resource, high, low = self.result()
        assert kind == RESULT_OWNERSHIP
        return resource, (high << U32_BITS) | low

    def lookup(self, lifetime):
        self.exercise(LOOKUP, lifetime >> U32_BITS, lifetime & 0xffffffff)
        return self.result()

    def disconnected(self):
        try:
            while self.connection.recv(X11_REPLY_SIZE):
                pass
        except ConnectionResetError:
            pass


def geometry(x11, resource, exists):
    x11.sendall(struct.pack("<BBHI", GET_GEOMETRY, 0, 2, resource))
    reply = receive_exact(x11, X11_REPLY_SIZE)
    if exists:
        assert reply[0] == 1, reply
    else:
        assert reply[0] == 0 and reply[1] == BAD_DRAWABLE, reply


def main():
    host, module = sys.argv[1:3]
    with tempfile.TemporaryDirectory(prefix="meatball-clients-") as directory:
        path = Path(directory) / "wayland"
        with server(host, module, {"MEATBALL_TEST_SOCKET": str(path)}) as (process, display):
            with connect_x11(display) as x11:
                wait_socket(path, process)
                observer = TestClient(path)
                try:
                    victim = TestClient(path)
                    resource, lifetime = victim.ownership()
                    assert observer.lookup(lifetime) == (RESULT_LOOKUP, 1, 0, 0)
                    geometry(x11, resource, True)
                    x11.sendall(struct.pack("<BBHI", KILL_CLIENT, 0, 2, resource))
                    victim.disconnected()
                    victim.close()
                    geometry(x11, resource, False)
                    assert observer.lookup(lifetime) == (RESULT_LOOKUP, 0, 0, 0)

                    replacement = TestClient(path)
                    _, replacement_lifetime = replacement.ownership()
                    assert replacement_lifetime != lifetime
                    assert observer.lookup(lifetime) == (RESULT_LOOKUP, 0, 0, 0)
                    replacement.exercise(OUTPUT)
                    for stage in range(1, OUTPUT_STAGES + 1):
                        assert replacement.result() == (RESULT_OUTPUT, stage, 0, 0)
                    replacement.close()
                    observer.sync()
                    assert observer.lookup(replacement_lifetime) == (RESULT_LOOKUP, 0, 0, 0)

                    for failure in [OVERSIZED, OVERFLOW, EXCEPTION, ORPHAN_FD,
                                    NATIVE_CLOSE, CLOSE_IN_OUTPUT, FD_OVERFLOW]:
                        victim = TestClient(path)
                        resource, lifetime = victim.ownership()
                        victim.exercise(failure)
                        victim.disconnected()
                        victim.close()
                        geometry(x11, resource, False)
                        assert observer.lookup(lifetime) == (RESULT_LOOKUP, 0, 0, 0)
                        if failure == ORPHAN_FD:
                            observer.exercise(FD_CLOSED)
                            assert observer.result() == (RESULT_FD_CLOSED, 0, 0, 0)
                    observer.sync()
                    victim = TestClient(path)
                    resource, lifetime = victim.ownership()
                    process.send_signal(signal.SIGHUP)
                    assert x11.recv(1) == b""
                    victim.disconnected()
                    victim.close()
                    observer.disconnected()
                    observer.close()
                    with connect_x11(display) as after_reset:
                        wait_socket(path, process)
                        observer = TestClient(path)
                        _, new_lifetime = observer.ownership()
                        assert new_lifetime != lifetime
                        assert observer.lookup(lifetime) == (RESULT_LOOKUP, 0, 0, 0)
                        geometry(after_reset, resource, False)
                finally:
                    observer.close()


if __name__ == "__main__":
    main()
