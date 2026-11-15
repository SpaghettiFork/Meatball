"""Locate headers and link inputs from a configured Spaghetti build."""

import json
from pathlib import Path
import shlex
import subprocess
import sys


XVFB_TARGET = "hw/vfb/Xvfb"
SDK_METADATA = Path("meson-info/intro-installed.json")


def xvfb_command(build):
    result = subprocess.run(
        ["ninja", "-C", str(build), "-t", "commands", XVFB_TARGET],
        check=True, capture_output=True, text=True,
    )
    for line in reversed(result.stdout.splitlines()):
        command = shlex.split(line)
        if "-o" in command and command[command.index("-o") + 1] == XVFB_TARGET:
            return command
    raise ValueError("Build Spaghetti's Xvfb target before configuring Meatball")


def main():
    build = Path(sys.argv[1]).resolve()
    mode = sys.argv[2]
    if mode == "includes":
        metadata = build / SDK_METADATA
        if not metadata.is_file():
            raise ValueError(
                f"{build} is not a configured Spaghetti build directory: "
                f"{SDK_METADATA} is missing. Set spaghetti_build_dir to the "
                "directory created by Meson when building Spaghetti."
            )
        installed = json.loads(metadata.read_text())
        sdk = next((Path(target).parent for target in installed.values()
                    if Path(target).name == "xorg-server.h"), None)
        if sdk is None:
            raise ValueError(f"{metadata} does not contain Spaghetti's xorg-server.h SDK header")
        headers = [Path(source) for source, target in installed.items()
                   if Path(target).parent == sdk and Path(source).suffix == ".h"]
        names = {header.name for header in headers}
        required = {"external.h", "xorg-server.h", "xf86Module.h", "present.h"}
        missing = required - names
        if missing:
            raise ValueError(f"Spaghetti SDK headers missing: {sorted(missing)}")
        directories = [build / "include", *sorted({header.parent for header in headers})]
        for directory in dict.fromkeys(directories):
            print(directory)
    elif mode == "inputs":
        for argument in xvfb_command(build):
            if argument.endswith((".o", ".a", ".so")):
                path = (build / argument).resolve()
                if not path.is_file():
                    raise ValueError(f"Build Spaghetti first: {path} is missing")
                print(path)
    else:
        raise ValueError(f"Unknown SDK query: {mode}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Spaghetti SDK discovery failed: {error}", file=sys.stderr)
        sys.exit(1)
