"""Minimal core Wayland requests for native-host integration checks."""

from array import array
import socket
import struct

from host_support import receive_exact, STARTUP_TIMEOUT

WAYLAND_DISPLAY_ID = 1
WAYLAND_HEADER_SIZE = 8
WAYLAND_MESSAGE_LIMIT = 1 << 20
DISPLAY_SYNC = 0
DISPLAY_GET_REGISTRY = 1
DISPLAY_ERROR = 0
CALLBACK_DONE = 0
REGISTRY_GLOBAL = 0


class WaylandProtocolError(RuntimeError):
    def __init__(self, payload):
        self.object_id, self.code, length = struct.unpack_from("=III", payload)
        super().__init__(payload[12:12 + length - 1].decode(errors="replace"))


class Wayland:
    def __init__(self, path):
        self.connection = socket.socket(socket.AF_UNIX)
        self.connection.settimeout(STARTUP_TIMEOUT)
        self.connection.connect(str(path))
        self.next_id = WAYLAND_DISPLAY_ID + 1

    def close(self):
        self.connection.close()

    def disconnected(self):
        try:
            while self.connection.recv(WAYLAND_HEADER_SIZE):
                pass
        except ConnectionResetError:
            pass

    def allocate_id(self):
        result = self.next_id
        self.next_id += 1
        return result

    def send(self, object_id, opcode, payload=b"", fds=()):
        packet = struct.pack("=II", object_id,
                             ((WAYLAND_HEADER_SIZE + len(payload)) << 16) | opcode) + payload
        if fds:
            sent = self.connection.sendmsg([packet], [(socket.SOL_SOCKET, socket.SCM_RIGHTS,
                                                       array("i", fds))])
            packet = packet[sent:]
        if packet:
            self.connection.sendall(packet)

    def receive(self):
        object_id, size_opcode = struct.unpack("=II", receive_exact(
                                              self.connection, WAYLAND_HEADER_SIZE))
        size, opcode = size_opcode >> 16, size_opcode & 0xffff
        assert WAYLAND_HEADER_SIZE <= size <= WAYLAND_MESSAGE_LIMIT
        payload = receive_exact(self.connection, size - WAYLAND_HEADER_SIZE)
        if object_id == WAYLAND_DISPLAY_ID and opcode == DISPLAY_ERROR:
            raise WaylandProtocolError(payload)
        return object_id, opcode, payload

    def sync(self):
        self.roundtrip_events()

    def roundtrip_events(self):
        callback = self.allocate_id()
        self.send(WAYLAND_DISPLAY_ID, DISPLAY_SYNC, struct.pack("=I", callback))
        events = []
        while True:
            event = self.receive()
            object_id, opcode, _ = event
            if object_id == callback:
                assert opcode == CALLBACK_DONE
                return events
            events.append(event)

    def globals(self):
        registry, callback = self.allocate_id(), self.allocate_id()
        self.send(WAYLAND_DISPLAY_ID, DISPLAY_GET_REGISTRY, struct.pack("=I", registry))
        self.send(WAYLAND_DISPLAY_ID, DISPLAY_SYNC, struct.pack("=I", callback))
        self.registry_id = registry
        globals_found = {}
        while True:
            object_id, opcode, payload = self.receive()
            if object_id == callback:
                assert opcode == CALLBACK_DONE
                return globals_found
            if object_id == registry and opcode == REGISTRY_GLOBAL:
                name, length = struct.unpack_from("=II", payload)
                version_offset = 8 + ((length + 3) & ~3)
                version, = struct.unpack_from("=I", payload, version_offset)
                globals_found[payload[8:8 + length - 1].decode("ascii")] = (name, version)

    def bind(self, interface, version=None):
        globals_found = self.globals()
        name, advertised_version = globals_found[interface]
        if version is None:
            version = advertised_version
        assert 0 < version <= advertised_version
        object_id = self.allocate_id()
        encoded = interface.encode("ascii") + b"\0"
        padded = encoded + bytes(-len(encoded) % 4)
        self.send(self.registry_id, 0, struct.pack("=II", name, len(encoded))
                  + padded + struct.pack("=II", version, object_id))
        return object_id
