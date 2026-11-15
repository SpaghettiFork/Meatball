"""Parse the active server keymap and check immutable descriptor ownership."""

import ctypes
import errno
import fcntl
import os
from pathlib import Path
import struct
import sys
import tempfile

from host_support import server
from native_lifecycle import wait_socket
from wayland_support import Wayland

KEY_Y = 29
XKB_CONTEXT_NO_FLAGS = 0
XKB_KEYMAP_FORMAT_TEXT_V1 = 1
XKB_KEYMAP_COMPILE_NO_FLAGS = 0
TEST_INTERFACE = "meatball_keymap_test"


class Keymaps:
    def __init__(self):
        self.library = ctypes.CDLL("libxkbcommon.so.0")
        self.library.xkb_context_new.argtypes = [ctypes.c_int]
        self.library.xkb_context_new.restype = ctypes.c_void_p
        self.library.xkb_context_unref.argtypes = [ctypes.c_void_p]
        self.library.xkb_keymap_new_from_string.argtypes = [ctypes.c_void_p, ctypes.c_char_p,
                                                         ctypes.c_int, ctypes.c_int]
        self.library.xkb_keymap_new_from_string.restype = ctypes.c_void_p
        self.library.xkb_keymap_unref.argtypes = [ctypes.c_void_p]
        self.library.xkb_keymap_key_get_syms_by_level.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                                               ctypes.c_uint32, ctypes.c_uint32,
                                                               ctypes.POINTER(ctypes.POINTER(ctypes.c_uint32))]
        self.library.xkb_keymap_key_get_syms_by_level.restype = ctypes.c_int
        self.context = self.library.xkb_context_new(XKB_CONTEXT_NO_FLAGS)
        assert self.context

    def close(self):
        self.library.xkb_context_unref(self.context)

    def symbol(self, text, keycode):
        keymap = self.library.xkb_keymap_new_from_string(self.context, text,
                                                      XKB_KEYMAP_FORMAT_TEXT_V1,
                                                      XKB_KEYMAP_COMPILE_NO_FLAGS)
        assert keymap, "server keymap did not compile"
        try:
            symbols = ctypes.POINTER(ctypes.c_uint32)()
            count = self.library.xkb_keymap_key_get_syms_by_level(keymap, keycode, 0, 0,
                                                                 ctypes.byref(symbols))
            assert count == 1
            return symbols[0]
        finally:
            self.library.xkb_keymap_unref(keymap)


def main():
    host, module = sys.argv[1:]
    parser = Keymaps()
    try:
        with tempfile.TemporaryDirectory(prefix="meatball-keymap-") as directory:
            path = Path(directory) / "wayland"
            with server(host, module, {"MEATBALL_TEST_SOCKET": str(path)}) as (process, _):
                wait_socket(path, process)
                client = Wayland(path)
                try:
                    test = client.bind(TEST_INTERFACE)
                    for layout, expected in ((0, ord("y")), (1, ord("z")), (0, ord("y"))):
                        client.send(test, 0, struct.pack("=I", layout))
                        while True:
                            object_id, opcode, data = client.receive()
                            if object_id == test:
                                assert opcode == 0
                                break
                        size, device = struct.unpack("=II", data)
                        assert device > 0 and len(client.fds) == 1
                        fd = client.fds.pop(0)
                        try:
                            assert fcntl.fcntl(fd, fcntl.F_GETFD) & fcntl.FD_CLOEXEC
                            assert os.fstat(fd).st_size == size
                            text = os.pread(fd, size, 0)
                            assert text.endswith(b"\0")
                            assert parser.symbol(text, KEY_Y) == expected
                            try:
                                os.pwrite(fd, b"x", 0)
                                raise AssertionError("keymap descriptor allowed writes")
                            except OSError as error:
                                assert error.errno == errno.EPERM
                        finally:
                            os.close(fd)
                    client.sync()
                finally:
                    client.close()
    finally:
        parser.close()


if __name__ == "__main__":
    main()
