"""Replace the socket pathname while native initialization sets permissions."""

import os
from pathlib import Path
import stat
import sys
import tempfile

from host_support import connect_x11, server
from native_lifecycle import SOCKET_MODE

TARGET_MODE = 0o640
SOCKET_DIRECTORY_MODE = 0o700
SOCKET_DIRECTORY_UID = 65534
SOCKET_DIRECTORY_GID = 65534


def main():
    host, module = sys.argv[1:3]
    with tempfile.TemporaryDirectory(prefix="meatball-socket-") as directory:
        root = Path(directory)
        runtime = root / "runtime"
        runtime.mkdir(mode=SOCKET_DIRECTORY_MODE)
        if os.geteuid() == 0:
            os.chown(runtime, SOCKET_DIRECTORY_UID, SOCKET_DIRECTORY_GID)
        path, backup, target = runtime / "wayland", runtime / "original", root / "target"
        target.write_bytes(b"unchanged")
        target.chmod(TARGET_MODE)
        target_before = target.stat()
        with server(host, module, {"MEATBALL_TEST_SOCKET": str(path),
                                  "MEATBALL_TEST_BACKUP": str(backup),
                                  "MEATBALL_TEST_TARGET": str(target)}) as (_, display):
            with connect_x11(display):
                assert path.is_symlink()
                assert backup.is_socket()
                assert stat.S_IMODE(backup.stat().st_mode) == SOCKET_MODE
                assert backup.stat().st_uid == runtime.stat().st_uid
                assert backup.stat().st_gid == runtime.stat().st_gid
                assert target.stat().st_uid == target_before.st_uid
                assert target.stat().st_gid == target_before.st_gid
                assert stat.S_IMODE(target.stat().st_mode) == TARGET_MODE
                assert target.read_bytes() == b"unchanged"
        assert not path.is_symlink()


if __name__ == "__main__":
    main()
