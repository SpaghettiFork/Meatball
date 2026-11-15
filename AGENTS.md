# Meatball.

A Wayland implementation inside Spaghetti, based upon 12to11.

## Supported platforms.

The native host currently targets Linux and uses `/proc/self/fd` when setting
socket permissions.

Meatball uses Xorg Render and Present. Rendering acceleration follows the
capabilities of the server's video DDX. Dmabuf buffers require DRI3 import
support from that DDX.

## Enabling Meatball

The native module currently handles core Wayland requests and version 1
`wl_compositor` surfaces on Spaghetti's server thread, with native window
ownership and committed input regions. Buffer rendering, shell roles, and
Wayland input are being ported and remain unavailable.

Load the `sauce` extension module in Spaghetti's `Module` section. Its
`SocketPath` option accepts an absolute Unix socket path; by default it uses
`$XDG_RUNTIME_DIR/wayland-meatball-<X display>`. `SocketMode` accepts octal
permissions and defaults to `0600`. The parent directory must already exist.
When running as root, the module assigns the socket to that directory's owner
and group. Clients select the absolute path through `WAYLAND_DISPLAY`.

Configuration ABI 2.0 replaces the previous process and renderer options with
`socket_path` and `socket_mode`. Module setup registers the `MEATBALL` X
extension. Its X protocol dispatcher returns `BadRequest`; Wayland clients
communicate through the configured socket.

## Building against Spaghetti

Use the SDK from the Spaghetti build that will load the module:

```sh
meson setup src/builddir src \
    -Dspaghetti_build_dir=/media/VMs/SpaghettiDev/build
meson compile -C src/builddir
meson test -C src/builddir --print-errorlogs
```

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
