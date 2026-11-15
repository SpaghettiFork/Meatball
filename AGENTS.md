# Meatball.

A Wayland implementation inside Spaghetti, based upon 12to11.

## Supported platforms.

The native host currently targets Linux and uses `/proc/self/fd` when setting
socket permissions.

The current renderer uses Xorg Render. Rendering acceleration follows the
capabilities of the server's video DDX. Present and DRI3 dmabuf imports are
part of the remaining port.

## Enabling Meatball

The native module currently handles core Wayland requests and version 4
`wl_compositor` surfaces on Spaghetti's server thread, with native window
ownership and committed input regions. SHM buffers use native imports and
Render with an independent server copy for redraws. Buffer scales, all eight
buffer transforms, and buffer-coordinate damage are supported. Version 1 xdg
toplevels support configure acknowledgements, titles, application IDs, parent windows,
size hints, WM state requests, close requests, unmapping, and remapping.

This is an initial rendering checkpoint. Subsurfaces, Present,
DRI3 buffers, Wayland input, output globals, popups, clipboard, drag and drop,
settings, and leases remain to be ported. Frame callbacks currently use a
16 ms server-loop timer. Text-input-v3 is deferred.

Load the `sauce` extension module in Spaghetti's `Module` section. Its
`SocketPath` option accepts an absolute Unix socket path; by default it uses
`$XDG_RUNTIME_DIR/wayland-meatball-<X display>`. `SocketMode` accepts octal
permissions and defaults to `0600`. The parent directory must already exist.
When running as root, the module assigns the socket to that directory's owner
and group. Clients select the absolute path through `WAYLAND_DISPLAY`.

Configure module options through a `sauce` subsection inside the existing
`Module` section:

```conf
Section "Module"
    SubSection "sauce"
        Option "SocketPath" "/run/user/1000/wayland-meatball-0"
        Option "SocketMode" "0600"
    EndSubSection
EndSection
```

Use your session user's runtime directory; `1000` is the example UID. Preserve
other module entries in that section. The subsection loads Sauce with its
options, so replace an existing `Load "sauce"` entry with this subsection.
Place `EndSubSection` before the enclosing module's `EndSection`.
Use a literal absolute path in `SocketPath`. For automatic socket naming,
set `XDG_RUNTIME_DIR` in the environment used to launch Xorg.

After starting Spaghetti with this configuration, launch clients with:

```sh
WAYLAND_DISPLAY=/run/user/1000/wayland-meatball-0 your-wayland-client
```

Configuration ABI 2.0 replaces the previous process and renderer options with
`socket_path` and `socket_mode`. Module setup registers the `MEATBALL` X
extension. Its X protocol dispatcher returns `BadRequest`; Wayland clients
communicate through the configured socket.

## Building against Spaghetti

Use the SDK from the Spaghetti build that will load the module:

```sh
cd /media/VMs/MeatballDev
meson compile -C /media/VMs/SpaghettiDev/build
meson setup src/builddir src \
    -Dspaghetti_build_dir=/media/VMs/SpaghettiDev/build
meson compile -C src/builddir
meson test -C src/builddir --print-errorlogs
```

`spaghetti_build_dir` names Spaghetti's configured build directory. It must
contain `meson-info/intro-installed.json`. The source checkout itself is a
different directory. For an already configured Meatball build, use
`meson configure src/builddir -Dspaghetti_build_dir=/media/VMs/SpaghettiDev/build`.
The resulting module is `src/builddir/libsauce.so`.

The SDK must include the 32-bit native SHM stride parameter and the exported
XACE send-access hook on Spaghetti's `meatball` branch. The configuration
check validates the SHM signature, and the
SDK probe validates the required exports.

Run the server built from that tree together with the newly built module.
To select both when launching through `startx`:

```sh
startx -- /media/VMs/SpaghettiDev/build/hw/xfree86/Xorg \
    -modulepath /media/VMs/MeatballDev/src/build,/usr/lib/xorg/modules
```

Here `src/build` is the Meatball build directory containing `libsauce.so`;
use `src/builddir` if following the build example above. Sauce resolves its
server API imports when loaded, so missing exports produce a module-load
error.

Build Spaghetti with Xvfb enabled first. The test host relinks its existing
objects with a module-loading hook and runs a separate CPU-backed X server.
The `native-sdk` test loads a probe for the required server exports and checks
an X11 client connection. `native-lifecycle` loads the actual module and checks
resets, socket cleanup, failed startup, and conflicting listeners. Tests need
permission to create local Unix sockets.
`native-loop` checks Wayland round trips alongside X11 traffic and a client
that leaves replies unread. `native-scheduler` exercises Wayland idle handlers,
timers, pipe reads, hangups, and source removal inside the server loop.
`native-clients` checks credentials, native resource ownership, X11
`KillClient`, deferred replies and events, FD transfer and cleanup, exceptional
clients, and lifetime identifiers across disconnects and resets.
`native-socket` replaces the socket pathname during permission setup and checks
that ownership and mode changes apply to the original socket inode.
`native-surfaces` checks surface windows, copied input regions, pending
callbacks, destruction, and reset cleanup.
`native-shm` checks imports, wide strides, buffer release, pixel readback, and
redraws after the client reuses its storage. `native-shm-fault` truncates an
imported file during rendering to exercise Spaghetti's SIGBUS handling.
`native-xdg` checks mapped pixel content, a test X11 window manager, configure
serials, WM messages and properties, size hints, parent inheritance, protocol
errors, remapping, and resets.
`native-transforms` checks all buffer orientations, integer scales, committed
geometry, retained pixels after release, and invalid surface state.

The integration checks run on the CPU Xvfb test host. The module also passes
these checks with address and undefined-behavior sanitizers; leak detection is
disabled for that host.

An installed matching SDK can be selected through `pkg-config` by omitting
`spaghetti_build_dir`. The isolated module test host requires the build tree.

## License

```
Spaghetti Display Server
Copyright (C)       SpaghettiFork

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
```

```
12to11, a Wayland compositor running on top of an X server.
Copyright (C) 2022 to various contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
```
