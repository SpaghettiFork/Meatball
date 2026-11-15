# Meatball.

A Wayland implementation on X11, based upon 12to11 that lets Spaghetti users sleep with ease.

## Supported platforms.

Meatball uses Xorg Render and Present. Rendering acceleration follows the
capabilities of the server's video DDX. Dmabuf buffers require DRI3 import
support from that DDX.

## Enabling Meatball

As of this commit, Meatball is not yet available.

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
an X11 client connection. Tests need permission to create local Unix sockets.

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
