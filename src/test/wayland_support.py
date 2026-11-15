"""Minimal core Wayland requests for native-host integration checks."""

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


class Wayland:
    def __init__(self, path):
        self.connection = socket.socket(socket.AF_UNIX)
        self.connection.settimeout(STARTUP_TIMEOUT)
        self.connection.connect(str(path))
        self.next_id = WAYLAND_DISPLAY_ID + 1

    def close(self):
        self.connection.close()

    def allocate_id(self):
        result = self.next_id
        self.next_id += 1
        return result

    def send(self, object_id, opcode, payload=b""):
        self.connection.sendall(struct.pack("=II", object_id,
                                ((WAYLAND_HEADER_SIZE + len(payload)) << 16) | opcode)
                                + payload)

    def receive(self):
        object_id, size_opcode = struct.unpack("=II", receive_exact(
                                              self.connection, WAYLAND_HEADER_SIZE))
        size, opcode = size_opcode >> 16, size_opcode & 0xffff
        assert WAYLAND_HEADER_SIZE <= size <= WAYLAND_MESSAGE_LIMIT
        payload = receive_exact(self.connection, size - WAYLAND_HEADER_SIZE)
        if object_id == WAYLAND_DISPLAY_ID and opcode == DISPLAY_ERROR:
            raise RuntimeError(f"Wayland protocol error: {payload!r}")
        return object_id, opcode, payload

    def sync(self):
        callback = self.allocate_id()
        self.send(WAYLAND_DISPLAY_ID, DISPLAY_SYNC, struct.pack("=I", callback))
        while True:
            object_id, opcode, _ = self.receive()
            if object_id == callback:
                assert opcode == CALLBACK_DONE
                return

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

    def bind(self, interface):
        globals_found = self.globals()
        name, version = globals_found[interface]
        object_id = self.allocate_id()
        encoded = interface.encode("ascii") + b"\0"
        padded = encoded + bytes(-len(encoded) % 4)
        self.send(self.registry_id, 0, struct.pack("=II", name, len(encoded))
                  + padded + struct.pack("=II", version, object_id))
        return object_id
