"""Read native output state across RandR changes and independent X screens."""

from pathlib import Path
import struct
import sys
import tempfile

from host_support import server
from native_lifecycle import wait_socket
from wayland_support import Wayland

TEST_INTERFACE = "meatball_output_test"
XDG_OUTPUT_MANAGER = "zxdg_output_manager_v1"
GET_XDG_OUTPUT = 1
XDG_DESTROY = 0
LOGICAL_POSITION = 0
LOGICAL_SIZE = 1
XDG_DONE = 2
XDG_NAME = 3
XDG_DESCRIPTION = 4
GEOMETRY = 0
MODE = 1
DONE = 2
SCALE = 3
NAME = 4
DESCRIPTION = 5
GLOBAL_REMOVE = 1
OUTPUT_RELEASE = 0
MODE_CURRENT = 1
MODE_PREFERRED = 2
SPLIT = 0
ROTATION = 1
CONNECTION = 2
DESTROY_SECOND = 3
MODE_FLAGS = 4
POSITION = 5
NON_DESKTOP = 6
PHYSICAL = 7
OFF = 8
RR_INTERLACE = 1 << 4
RR_DOUBLE_SCAN = 1 << 5
WIDTH = 64
HEIGHT = 128
REFRESH = 60000


def string(data, offset=0):
    length, = struct.unpack_from("=I", data, offset)
    return data[offset + 4:offset + 4 + length - 1].decode(), offset + 4 + ((length + 3) & ~3)


def geometry(data):
    x, y, mm_width, mm_height, subpixel = struct.unpack_from("=iiiii", data)
    make, offset = string(data, 20)
    model, offset = string(data, offset)
    transform, = struct.unpack_from("=i", data, offset)
    return x, y, mm_width, mm_height, subpixel, make, model, transform


def last(events, resource, opcode):
    return [data for object_id, event, data in events
            if object_id == resource and event == opcode][-1]


class Outputs(Wayland):
    def outputs(self):
        self.globals()
        return [name for name, (interface, _) in self.global_entries.items()
                if interface == "wl_output"]

    def bind_output(self, name, version=4):
        resource = self.bind("wl_output", version, name=name)
        events = self.roundtrip_events()
        opcodes = [opcode for object_id, opcode, _ in events if object_id == resource]
        expected = [GEOMETRY, MODE]
        if version >= 2:
            expected += [SCALE]
        if version >= 4:
            expected += [NAME, DESCRIPTION]
        if version >= 2:
            expected += [DONE]
        assert opcodes == expected, opcodes
        assert geometry(last(events, resource, GEOMETRY))[5] == "Spaghetti"
        if version >= 2:
            assert struct.unpack("=i", last(events, resource, SCALE)) == (1,)
        return resource, events

    def change_outputs(self, operation, argument=0):
        self.send(self.test, 0, struct.pack("=II", operation, argument & 0xffffffff))
        return self.roundtrip_events() + self.roundtrip_events()

    def bind_xdg_output(self, output, version, position, size, metadata=None,
                        core_done=True):
        assert self.globals()[XDG_OUTPUT_MANAGER][1] == 3
        manager = self.bind(XDG_OUTPUT_MANAGER, version)
        resource = self.allocate_id()
        self.send(manager, GET_XDG_OUTPUT, struct.pack("=II", resource, output))
        self.send(manager, XDG_DESTROY)
        expected = [(resource, LOGICAL_POSITION, struct.pack("=ii", *position)),
                    (resource, LOGICAL_SIZE, struct.pack("=ii", *size))]
        events = [event for event in self.roundtrip_events()
                  if event[0] in (resource, output)]
        if version >= 2:
            assert [string(last(events, resource, opcode))[0]
                    for opcode in (XDG_NAME, XDG_DESCRIPTION)] == list(metadata)
            expected += [(resource, opcode, last(events, resource, opcode))
                         for opcode in (XDG_NAME, XDG_DESCRIPTION)]
        expected.append((output, DONE, b"") if version >= 3 and core_done
                        else (resource, XDG_DONE, b""))
        assert events == expected, events
        return resource


def check_basic(path):
    client = Outputs(path)
    try:
        names = client.outputs()
        assert len(names) == 1, names
        modern, events = client.bind_output(names[0])
        metadata = [string(last(events, modern, opcode))[0]
                    for opcode in (NAME, DESCRIPTION)]
        for version in range(1, 5):
            resource, events = client.bind_output(names[0], version)
            flags, width, height, refresh = struct.unpack("=Iiii", last(events, resource, MODE))
            assert flags & MODE_CURRENT and (width, height) == (128, 128) and refresh >= 0, (flags, width, height, refresh)
            if version >= 3:
                client.send(resource, OUTPUT_RELEASE)
            if version == 1:
                # A legacy core binding cannot receive wl_output.done.
                xdg = client.bind_xdg_output(resource, 3, (0, 0), (128, 128),
                                             metadata, core_done=False)
                client.send(xdg, XDG_DESTROY)
        for version in range(1, 4):
            xdg = client.bind_xdg_output(modern, version, (0, 0), (128, 128),
                                         metadata)
            client.send(xdg, XDG_DESTROY)
        client.send(modern, OUTPUT_RELEASE)
        client.sync()
    finally:
        client.close()


def check_changes(path):
    client = Outputs(path)
    try:
        client.test = client.bind(TEST_INTERFACE)
        first_name, = client.outputs()
        first, events = client.bind_output(first_name)
        stable_name, _ = string(last(events, first, NAME))
        metadata = [stable_name, string(last(events, first, DESCRIPTION))[0]]
        legacy = client.bind_xdg_output(first, 2, (0, 0), (128, 128), metadata)
        modern = client.bind_xdg_output(first, 3, (0, 0), (128, 128), metadata)
        events = client.change_outputs(SPLIT)
        assert struct.unpack("=Iiii", last(events, first, MODE)) == (
            MODE_CURRENT | MODE_PREFERRED, WIDTH, HEIGHT, REFRESH)
        assert not any(object_id == first and opcode == NAME for object_id, opcode, _ in events)
        changes = [event for event in events if event[0] in (legacy, modern)
                   or event[:2] == (first, DONE)]
        size = struct.pack("=ii", WIDTH, HEIGHT)
        assert changes == [(legacy, LOGICAL_SIZE, size), (legacy, XDG_DONE, b""),
                           (modern, LOGICAL_SIZE, size), (first, DONE, b"")], changes
        second_name, = set(client.outputs()) - {first_name}
        second, events = client.bind_output(second_name)
        assert geometry(last(events, second, GEOMETRY))[:2] == (WIDTH, 0)
        assert string(last(events, second, NAME))[0] != stable_name
        second_metadata = [string(last(events, second, opcode))[0]
                           for opcode in (NAME, DESCRIPTION)]
        client.bind_xdg_output(second, 3, (WIDTH, 0), (WIDTH, HEIGHT),
                               second_metadata)
        # Expected transforms for rotations with no reflection, X, Y, and both.
        expected_transforms = ((0, 1, 2, 3), (4, 7, 6, 5),
                               (6, 5, 4, 7), (2, 3, 0, 1))
        previous_size = (WIDTH, HEIGHT)
        for reflection, transforms in zip((0, 16, 32, 48), expected_transforms):
            for rotation, expected in zip((1, 2, 4, 8), transforms):
                events = client.change_outputs(ROTATION, reflection | rotation)
                size = (HEIGHT, WIDTH) if rotation in (2, 8) else (WIDTH, HEIGHT)
                size_events = [data for object_id, opcode, data in events
                               if (object_id, opcode) == (modern, LOGICAL_SIZE)]
                assert size_events == ([struct.pack("=ii", *size)]
                                       if size != previous_size else [])
                previous_size = size
                if events and any(object_id == first and opcode == GEOMETRY
                                  for object_id, opcode, _ in events):
                    assert geometry(last(events, first, GEOMETRY))[-1] == expected
                    assert last(events, first, DONE) == b""
                else:
                    assert reflection == 0 and rotation == 1
        for flags, expected in ((RR_INTERLACE, REFRESH * 2),
                                (RR_DOUBLE_SCAN, REFRESH // 2),
                                (RR_INTERLACE | RR_DOUBLE_SCAN, REFRESH), (0, REFRESH)):
            events = client.change_outputs(MODE_FLAGS, flags)
            modes = [data for object_id, opcode, data in events if object_id == first and opcode == MODE]
            if modes:
                assert struct.unpack("=Iiii", modes[-1])[-1] == expected
            else:
                assert flags == 0 and expected == REFRESH
        events = client.change_outputs(POSITION, -WIDTH)
        assert geometry(last(events, first, GEOMETRY))[:2] == (-WIDTH, 0)
        assert struct.unpack("=ii", last(events, modern, LOGICAL_POSITION)) == (-WIDTH, 0)
        events = client.change_outputs(PHYSICAL)
        assert geometry(last(events, first, GEOMETRY))[2:5] == (320, 180, 2)
        assert not any(event[0] in (legacy, modern) for event in events)
        events = client.change_outputs(CONNECTION, 0)
        assert any(opcode == GLOBAL_REMOVE and struct.unpack("=I", data) == (first_name,)
                   for object_id, opcode, data in events if object_id != client.test)
        assert client.outputs() == [second_name]
        # A client can bind a global announced before its removal reached it.
        retired = client.allocate_id()
        encoded = b"wl_output\0"
        padded = encoded + bytes(-len(encoded) % 4)
        client.send(client.registry_id, 0,
                    struct.pack("=II", first_name, len(encoded)) + padded
                    + struct.pack("=II", 4, retired))
        events = client.roundtrip_events()
        assert struct.unpack("=Iiii", last(events, retired, MODE))[1:3] == (WIDTH, HEIGHT)
        client.send(retired, OUTPUT_RELEASE)
        # A removed binding stays valid until the client releases it.
        client.send(first, OUTPUT_RELEASE)
        client.sync()
        events = client.change_outputs(CONNECTION, 1)
        assert not any(event[0] in (legacy, modern) for event in events)
        client.send(legacy, XDG_DESTROY)
        client.send(modern, XDG_DESTROY)
        replacement_name, = set(client.outputs()) - {second_name}
        assert replacement_name != first_name
        first, events = client.bind_output(replacement_name)
        assert string(last(events, first, NAME))[0] == stable_name
        client.change_outputs(NON_DESKTOP, 1)
        assert client.outputs() == [second_name]
        client.change_outputs(NON_DESKTOP, 0)
        assert len(client.outputs()) == 2
        client.change_outputs(DESTROY_SECOND)
        assert len(client.outputs()) == 1
        client.change_outputs(OFF)
        assert client.outputs() == []
        # Existing RandR outputs with no active CRTCs produce no fallback output.
        client.change_outputs(SPLIT)
        assert len(client.outputs()) == 2
        client.send(second, OUTPUT_RELEASE)
        client.sync()
    finally:
        client.close()


def check_screens(path):
    client = Outputs(path)
    try:
        sizes, names = [], []
        for name in client.outputs():
            resource, events = client.bind_output(name)
            sizes.append(struct.unpack("=Iiii", last(events, resource, MODE))[1:3])
            names.append(string(last(events, resource, NAME))[0])
        assert sorted(sizes) == [(96, 64), (128, 128)], sizes
        assert len(set(names)) == 2
    finally:
        client.close()


def main():
    host, module, probe = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="meatball-outputs-") as directory:
        path = Path(directory) / "wayland"
        for plugin, arguments, check in (
                (module, (), check_basic),
                (module, ("-extension", "RANDR"), check_basic),
                (module, ("-screen", "1", "96x64x24", "-crtcs", "1@96x64", "-extension", "XINERAMA"), check_screens),
                (probe, (), check_changes)):
            with server(host, plugin, {"MEATBALL_TEST_SOCKET": str(path)},
                        arguments=("-crtcs", "1@128x128", *arguments)) as (process, display):
                wait_socket(path, process)
                check(path)
                assert process.poll() is None


if __name__ == "__main__":
    main()
