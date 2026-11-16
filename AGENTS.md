# Meatball agent notes.

Meatball is a Wayland implementation inside the Spaghetti X server, based upon
12to11. This file holds the working knowledge about the repository: how the
tree is laid out, how to build and test it, how the module is configured, what
is implemented, and what is still missing.

The user-facing README covers the introduction, the build snippet, and the
license. Keep new implementation notes here rather than in the README.

## Repository layout

```
src/meson.build           project definition, options, sauce module target
src/meson_options.txt     module_dir, spaghetti_build_dir, tests
src/sauce.c               Spaghetti module entry point and Sauce options
src/meatball/             the compositor
src/test/                 module test host, probes, and native-* checks
.old/                     retired 12to11 sources, kept locally and ignored
```

Inside `src/meatball/`:

```
meatball.c/.h             initialization, socket creation, shutdown
run.c                     server loop integration
native_client.c/.h        per-connection native resources and FD transfer
native_cursor.c/.h        client cursor surfaces as native cursors
native_data.c/.h          clipboard and drag-and-drop transfers
native_input.c/.h         pointer and keyboard routing, grabs
native_keymap.c/.h        sealed keymap descriptors
native_output.c/.h        wl_output globals and surface membership
native_render.c           Render protocol drawing
native_shm.c/.h           SHM buffer import
native_subsurface.c/.h    subsurface tree and stacking
native_surface.c/.h       surfaces, roles, input regions
native_viewport.c/.h      viewport cropping and destination sizing
native_window.c/.h        native client windows
native_xdg.c/.h           xdg-shell toplevels
build_version.h.in        revision stamp template
wayland-proto/            protocol XML, scanned by wayland-scanner
```

`src/meatball/wayland-proto/` holds the upstream protocol definitions. Only
`xdg-shell.xml` and `viewporter.xml` are scanned into the build today.

`src/test/` contains the isolated module test host (`build_host.py`,
`host.c`, `host_smoke.py`, `host_support.py`), the SDK discovery and probe
helpers (`sdk_paths.py`, `sdk_probe.c`), the client-side probe programs
(`probe.c`, `client_probe.c`, `keymap_probe.c`, `loop_probe.c`,
`output_probe.c`, `shm_fault_probe.c`, `socket_probe.c`), the shared Wayland
client helpers (`wayland_support.py`), and one Python script per `native-*`
check.

## Building and testing

Use the SDK from the Spaghetti build that will load the module:

```sh
cd [path to Meatball repository]
meson compile -C [path to Spaghetti build]
meson setup src/builddir src \
    -Dspaghetti_build_dir=[path to Spaghetti build]
meson compile -C src/builddir
meson test -C src/builddir --print-errorlogs
```

`spaghetti_build_dir` names Spaghetti's configured build directory. It must
contain `meson-info/intro-installed.json`. The source checkout itself is a
different directory. For an already configured Meatball build, use
`meson configure src/builddir -Dspaghetti_build_dir=[path to Spaghetti build]`.
The resulting module is `src/builddir/libsauce.so`.

Build Spaghetti with Xvfb enabled first. The test host relinks its existing
objects with a module-loading hook and runs a separate CPU-backed X server.
Tests need permission to create local Unix sockets.

Startup logs include `meatball: Build <revision>` before the socket path. The
revision records the Git commit and a `-dirty` suffix for local tracked changes;
source archives use the project version. Compare this line in the running
Xorg log with the revision you built after installing the module and restarting
Spaghetti.

Run the server built from that tree together with the newly built module.
To select both when launching through `startx`:

```sh
startx -- [path to Spaghetti build]/hw/xfree86/Xorg \
    -modulepath [path to Meatball build]/src/builddir,/usr/lib/xorg/modules
```

Sauce resolves its server API imports when loaded, so missing exports produce
a module-load error.

### SDK requirements

The SDK must include the 32-bit native SHM stride parameter and the exported
XACE send-access hook, `XISetEventMask`, and `RT_XKBCLIENT` on Spaghetti's
`meatball` branch. The configuration check validates the SHM signature, and
the SDK probe validates the required exports, including `GrabDevice` and
`WindowHasNewCursor` for native drag grabs and cursor updates.

An installed matching SDK can be selected through `pkg-config` by omitting
`spaghetti_build_dir`. The isolated module test host requires the build tree.

The integration checks run on the CPU Xvfb test host. The module also passes
these checks with address and undefined-behavior sanitizers; leak detection is
disabled for that host.

## Module configuration

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

## Implementation status

The native host currently targets Linux and uses `/proc/self/fd` when setting
socket permissions.

The current renderer uses Xorg Render. Rendering acceleration follows the
capabilities of the server's video DDX.

### Compositor and surfaces

The native module handles core Wayland requests and version 4 `wl_compositor`
surfaces on Spaghetti's server thread, with native window ownership and
committed input regions. SHM buffers use native imports and Render with an
independent server copy for redraws. Buffer scales, all eight buffer
transforms, and buffer-coordinate damage are supported. Surface dimensions are
limited to 32767 in each direction.

Version 1 `wp_viewporter` supports cropping and destination sizing, including
fractional source rectangles.

### Toplevels

Version 1 xdg toplevels support configure acknowledgements, titles,
application IDs, parent windows, size hints, WM state requests, close
requests, unmapping, and remapping.

Toplevels use client-side decorations and request an undecorated X11 window
through `_MOTIF_WM_HINTS`. Window geometry supplies `_GTK_FRAME_EXTENTS` for
client-drawn shadows. Server-side decoration negotiation remains to be ported.
When the screen has no X11 compositor, the native window canvas excludes the
client's shadow margins while retaining visible subsurfaces outside the parent.
Compositor ownership changes update the canvas and input coordinates
automatically; a composited desktop retains the full alpha buffer and its
shadows. ICCCM base-size hints account for the remaining margins on composited
desktops. Maximized and fullscreen canvases follow the committed window
geometry, keeping transient status subsurfaces from changing the window size.
Window geometry changes preserve the visible window's position. Geometry and
configure acknowledgements apply with their corresponding surface commits.
Replies to older configures preserve the WM's latest native size until the
matching reply is committed. Repaints preserve unchanged input shapes, frame
extents, and size hints.

### Subsurfaces

Version 1 `wl_subcompositor` supports synchronized and desynchronized content
updates, nested subsurfaces, parent-committed positions, and stacking above or
below siblings and the parent. A surface tree renders into one native window,
including children outside the parent's buffer. The canvas is limited to
32767 pixels per direction and 256 MiB; each surface can queue 256 commits.
SHM pixels are copied when committed, and frame callbacks wait until the
corresponding surface state is visible in a mapped tree.

### Input

Version 5 `wl_seat` exposes each enabled Xorg master pointer/keyboard pair.
Pointer events follow committed surface-tree stacking and input regions, with
implicit button grabs and wheel scrolling. Keyboard events follow native X11
focus, with the server's current XKB keymap, modifiers, held keys, and repeat
settings. Keymap changes and master-device hotplug update existing clients.

Client cursor surfaces use native ARGB cursors, including scale, transform,
viewport, and subsurface composition. Cursor images and hotspot padding are
limited to 512 pixels per direction. Cursor frame callbacks use the surface
timer.

Toplevel move and resize requests validate the seat's active button-press
serial and transfer the implicit pointer grab to the X11 WM through
`_NET_WM_MOVERESIZE`.

### Outputs

Version 4 `wl_output` describes each connected, enabled RandR desktop output.
It reports geometry, physical size, the current mode and refresh rate, rotation
and reflection, subpixel order, a stable name, and a description. Output changes
and disconnects update existing clients. Screens without RandR output data use
the native X screen geometry. Separate X screens expose separate output globals;
surface windows currently use the first X screen.

Surfaces receive output enter/leave events as their committed content crosses
output boundaries. This includes subsurfaces, cursor images and cursor children,
window movement, minimize/restore, and output layout changes. Binding an output
after mapping a surface also sends its enter event. Output scale is currently 1.

### Clipboard and drag and drop

`wl_data_device_manager` supports clipboard and pointer drag-and-drop transfers
between Meatball's Wayland clients. It advertises version 4 with current Wayland
headers and version 3 with older headers. Clipboard ownership is per seat;
keyboard focus controls which client receives selection offers. Ownership
changes require a serial delivered to that client on that seat. Transfers pass
the destination's file descriptor directly to the source, including transfers
larger than a pipe buffer. The server loop remains responsive while a reader is
blocked. The selection clears when its source is destroyed.

Drag-and-drop validates the origin's implicit button grab, follows committed
surface-tree stacking and input regions, and negotiates copy, move, and ask
actions. Transfers work before and after the drop. Drag icons use native
windows with empty input shapes; their bounding shapes exclude fully
transparent pixels. Icons support buffer offsets, transforms, viewports, and
subsurfaces. Source cursor changes continue during the drag. Disconnects,
device removal, and server reset cancel pending transfers.

## Still to port

- X11 clipboard selections.
- Xdnd interoperability.
- Primary selection.
- Present.
- DRI3 buffers.
- Popups.
- Settings.
- Leases.
- Text-input-v3 is deferred.
- Server-side decoration negotiation.
- Frame callbacks currently use a 16 ms server-loop timer.

## Test inventory

Every check runs against the isolated module test host.

- `native-sdk` loads a probe for the required server exports and checks an X11
  client connection.
- `native-lifecycle` loads the actual module and checks resets, socket
  cleanup, failed startup, and conflicting listeners.
- `native-loop` checks Wayland round trips alongside X11 traffic and a client
  that leaves replies unread.
- `native-scheduler` exercises Wayland idle handlers, timers, pipe reads,
  hangups, and source removal inside the server loop.
- `native-clients` checks credentials, native resource ownership, X11
  `KillClient`, deferred replies and events, FD transfer and cleanup, exceptional
  clients, and lifetime identifiers across disconnects and resets.
- `native-socket` replaces the socket pathname during permission setup and checks
  that ownership and mode changes apply to the original socket inode.
- `native-surfaces` checks surface windows, copied input regions, pending
  callbacks, destruction, and reset cleanup.
- `native-shm` checks imports, wide strides, buffer release, pixel readback, and
  redraws after the client reuses its storage.
- `native-shm-fault` truncates an imported file during rendering to exercise
  Spaghetti's SIGBUS handling.
- `native-xdg` checks mapped pixel content, a test X11 window manager, configure
  serials, WM messages and properties, size hints, parent inheritance, protocol
  errors, remapping, and resets.
- `native-transforms` checks all buffer orientations, integer scales, committed
  geometry, retained pixels after release, and invalid surface state.
- `native-viewport` checks cropped pixels across buffer orientations and scales,
  fractional rectangles, commit timing, protocol errors, destruction, and resets.
- `native-shadows` checks desktop pixels around client shadow margins, preserved
  alpha, compositor ownership changes, resize size hints, geometry commits,
  remapping, and resets.
- `native-geometry` checks that input-driven repaints keep native geometry
  stable and only changed shapes and hints emit notifications. It also checks
  cropped input coordinates, visible window placement, status subsurfaces
  extending past maximized and fullscreen window edges, delayed configure
  replies, and compositor changes while a resize is pending.
- `native-subsurfaces` checks synchronized content, nested commit dependencies,
  parent-committed position and stacking, alpha, negative coordinates, input
  regions, buffer transforms and viewports, role lifetime, destruction, and reset.
- `native-keymap` parses sealed keymap descriptors and checks live layout
  changes.
- `native-input` injects XTEST pointer and keyboard events, exercises focus and
  implicit grabs, and checks modifiers, repeat controls, client isolation,
  subsurface destruction, and master-device hotplug.
- `native-cursor` reads back ARGB cursor images through XFixes.
- `native-interactive` checks move/resize serials, all resize directions, and
  pointer-grab transfer to a test WM.
- `native-outputs` checks output protocol versions, current modes, all RandR
  rotation/reflection combinations, physical metadata, disconnect/reconnect,
  non-desktop outputs, and multiple X screens.
- `native-output-surfaces` checks membership during movement, minimize/restore,
  synchronized child commits, cursor updates, output binding/removal, and server
  reset.
- `native-data` checks clipboard focus and serial validation, source
  replacement, MIME offers, large and blocked transfers, protocol versions,
  disconnects, and reset cleanup.
- `native-drag` checks native pointer grabs, destination movement,
  copy/move/ask negotiation, pre-drop and post-drop transfers, legacy clients,
  multiple device bindings, source-less drags, cancellation, role and action
  errors, cursor updates, shaped icons, icon reuse, disconnects, and resets.

The input checks use `libxkbcommon.so.0` to parse keymaps. XTEST is enabled
explicitly in the isolated input test servers. Multi-screen checks use
Spaghetti's Xvfb CRTC isolation fix (`85d4e8708`).

## Conventions

Commit subjects use `area: Summary` with a lowercase area prefix such as
`xdg`, `input`, `surfaces`, `data`, `outputs`, `clients`, `build`, `test`, or
`docs`. The body holds one or more paragraphs describing the behaviour and
the coverage. Do not use em dashes.

Format C with `clang-format` and the repository `.clang-format` before
committing. The style is GNU based with four space indentation, Allman
braces, and a 79 column limit.

Record implementation progress and repository knowledge in this file. Keep
the README limited to the introduction, the build snippet, and the license.

## X11 bridge work

The data-device backend supports native data sources through `MbDataSourceOps`.
The receive callback takes ownership of the destination FD. Source notifications
cover target, action, drop, completion, cancellation, and destruction; providers
must defer source destruction until notification handling returns. Native
sources share MIME validation and offer lifetime handling with Wayland sources.
`MbDataBridge` observes local selection changes and source destruction.
`mb_data_import_selection` publishes a foreign selection to active seats and
newly created data devices. These are private module interfaces; configuration
ABI 2.0 and the Xorg extension ABI remain unchanged.
