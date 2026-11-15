"""Relink the selected Xvfb with Meatball's module-loading hook."""

from pathlib import Path
import subprocess
import sys

from sdk_paths import xvfb_command


build = Path(sys.argv[1]).resolve()
hook = Path(sys.argv[2]).resolve()
output = Path(sys.argv[3]).resolve()
command = xvfb_command(build)
command[command.index("-o") + 1] = str(output)
command.insert(command.index("-Wl,--start-group") + 1, str(hook))
command.extend(["-Wl,--wrap=InitExtensions", "-Wl,--export-dynamic"])
command.extend(sys.argv[4:])
subprocess.run(command, cwd=build, check=True)
