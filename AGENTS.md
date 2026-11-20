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
native_selection.c/.h     asynchronous X11 selection conversions
native_xdnd.c/.h          X11 drag-and-drop negotiation
native_input.c/.h         pointer and keyboard routing, grabs
native_keymap.c/.h        sealed keymap descriptors
native_output.c/.h        wl_output globals and surface membership
native_render.c           Render protocol drawing
native_shm.c/.h           SHM buffer import
native_subsurface.c/.h    subsurface tree and stacking
native_surface.c/.h       surfaces, roles, input regions
native_viewport.c/.h      viewport cropping and destination sizing
native_buffer.c/.h        shared Wayland buffer ownership and pixmap snapshots
native_dmabuf.c/.h        Linux DMA-BUF imports through DRI3
native_window.c/.h        native client windows
native_xdg.c/.h           xdg-shell toplevels
build_version.h.in        revision stamp template
wayland-proto/            protocol XML, scanned by wayland-scanner
```

`src/meatball/wayland-proto/` holds the upstream protocol definitions.
The build scans xdg-shell, xdg-output-unstable-v1, viewporter,
primary-selection-unstable-v1, linux-dmabuf, pointer-constraints-unstable-v1,
relative-pointer-unstable-v1, tearing-control-v1, xdg-system-bell-v1, and
single-pixel-buffer-v1.

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
XACE send-access hook, `XISetEventMask`, `RT_XKBCLIENT`, and
`RelativeMotionCallback` on Spaghetti's `meatball` branch. The configuration
check validates the SHM signature and relative-motion declaration, and
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
permissions and defaults to `0600`. Sauce waits for the configured parent
directory to exist before starting Meatball. If the native bridge closes
or the socket pathname disappears, Sauce restarts Meatball in the current
X server generation once the directory is available again. Wayland clients
must reconnect after a restart.
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

Version 1 `wp_single_pixel_buffer_manager_v1` creates immutable 1×1 buffers
from premultiplied RGBA values. Render uses a solid source with 16-bit
channels, and viewporter can scale it to the committed surface size. The
buffer is released on commit and its color stays available for redraws
until the surface replaces or drops the content.

SHM additionally advertises RGB565 and XRGB1555 when the first X screen has
the corresponding Render formats. Packed 16-bit rows are copied into
X-aligned pixmaps before drawing.

The `zwp_linux_dmabuf_v1` global is available when the first X screen reports
DRI3 modifiers for XRGB8888, ARGB8888, or RGB565. Version 4 supplies default and
per-surface feedback when DRI3 opens a device for that screen. The feedback
contains the device number and a sealed format table in a single tranche for
compositing. If the device is unavailable, the global stays at version 3.
An asynchronous DRI3 reply can advertise version 4 for new bindings.
Meatball uses DRI3 to import the planes for both buffer creation requests.
XRGB1555 DMA-BUF needs exact format discovery and import support in Spaghetti.
Progressive DMA-BUF commits retain the DRI3 import as surface content. Render
samples the imported pixmap directly and handles Y inversion in its sampling
transform. The module submits queued reads through the screen's Sync fence
when the final committed or queued use ends. It sends `wl_buffer.release` once
the imported planes' implicit fences are ready and waits asynchronously when
they are busy. Destroying the Wayland buffer object leaves committed content
alive until the surface replaces or drops it.

### Toplevels

Xdg-shell is advertised at version 3. Toplevels support configure
acknowledgements, titles, application IDs, parent windows, size hints, WM state
requests, close requests, unmapping, and remapping.

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
On noncomposited desktops, subsurfaces with an explicitly empty input region
that overlap the window geometry paint without expanding the windowed canvas.
ARGB subsurfaces that extend beyond all four sides of the content also paint
without expanding the canvas. This clips libdecor's shadow and its outer resize
region while keeping the title bar inside the declared window geometry.
Window geometry changes preserve the visible window's position. Geometry and
configure acknowledgements apply with their corresponding surface commits.
Replies to older configures preserve the WM's latest native size until the
matching reply is committed. Repaints preserve unchanged input shapes, frame
extents, and size hints.

### Popups

Xdg-shell popups use override-redirect X11 windows and support nested menus.
Positioners place popups relative to the parent's committed
window geometry and apply flip, slide, and resize constraints against the
RandR output at the anchor. Popups follow parent movement and are dismissed
in reverse stacking order when a parent unmaps.

Explicit popup grabs validate a button or key serial from the parent and
acquire the seat's X11 pointer and keyboard devices. Pointer events continue
to reach surfaces owned by the same client, and the topmost grabbing popup
receives keyboard focus. Outside clicks on other X11 windows, Escape, and
seat removal dismiss the grab stack. Destroying a nested popup returns the
grab to its parent.

Version 3 clients can reposition a mapped popup. The server sends the request
token, the popup geometry, and the surface configure serial in that order.
The popup moves when the client commits the acknowledgement for that serial.
Reactive positioners recalculate constraints on parent movement and output
layout changes. Placement uses the current native parent origin and accepts
the optional parent size and configure hints without predicting future
parent geometry.

### Subsurfaces

Version 1 `wl_subcompositor` supports synchronized and desynchronized content
updates, nested subsurfaces, parent-committed positions, and stacking above or
below siblings and the parent. A surface tree renders into one native window,
including children outside the parent's buffer. The canvas is limited to
32767 pixels per direction and 256 MiB; each surface can queue 256 commits.
SHM pixels are copied when committed, and frame callbacks wait until the
corresponding surface state is visible in a mapped tree.

Mapped toplevels with frame callbacks render the scene into a pixmap at the
native window's depth and submit it with X11 Present in copy mode. One
submission per window may be pending. Commits arriving during the wait are
drawn in the next batch, and callbacks use the matching completion timestamp.
Updates without frame callbacks and updates during native size mismatches use
direct Render drawing. Where Present timing is available, direct drawing with
frame callbacks uses NotifyMSC. Cursor images and windows without usable
Present timing use a 16 ms timer. A window falls back to direct drawing and
the timer if a Present completion stalls for 1.5 seconds.

Version 1 `wp_tearing_control_manager_v1` applies its hints on surface
commit. A toplevel's committed asynchronous hint lets Present copy its
composed frame without waiting for vblank. Destroying the control resets
the hint to vsync on the next commit. Each X11 window uses the hint from
its root surface.

### Input

Version 5 `wl_seat` exposes each enabled Xorg master pointer/keyboard pair.
Pointer events follow committed surface-tree stacking and input regions, with
implicit button grabs and scrolling. XI2 touchpad valuators preserve
fractional scroll deltas and send both axes together as continuous motion.
Meatball omits the corresponding emulated wheel buttons. Discrete wheel
buttons use the existing wheel events. Keyboard events follow native X11
focus, with the server's current XKB keymap, modifiers, held keys, and repeat
settings. Keymap changes and master-device hotplug update existing clients.

Client cursor surfaces use native ARGB cursors, including scale, transform,
viewport, and subsurface composition. Cursor images and hotspot padding are
limited to 512 pixels per direction. Cursor frame callbacks use the surface
timer.

Toplevel move and resize requests validate the seat's active button-press
serial and transfer the implicit pointer grab to the X11 WM through
`_NET_WM_MOVERESIZE`.

Version 1 `zwp_pointer_constraints_v1` supports locking and confinement,
including one-shot and persistent lifetimes. A constraint activates when
its surface has pointer focus within the effective region and the native
window has seat keyboard focus. Input-only X11 windows constrain the native
cursor through XI2 grabs. Constraint regions intersect the committed surface
input region and take effect with the corresponding surface commit, including
synchronized subsurface commits. Locked pointers suppress absolute motion and
continue delivering buttons and scroll events. Cursor-position hints are
accepted. Unlocking keeps the current cursor position. Constraints release
on focus loss, unmap, or device retirement and yield to native menu, drag,
and move or resize grabs.

Version 1 `zwp_relative_pointer_manager_v1` sends accelerated and
unaccelerated Xorg motion deltas to the pointer-focused client. Deltas are
captured before cursor clipping, including during locks and confinement.
Absolute device motion, cursor warps, scrolling, and motion under a foreign
X11 grab produce no relative events. Relative pointers follow their seat's
focus and become inert when it is removed. Each relative sample ends with
`wl_pointer.frame` on supported pointer bindings, including while a lock
suppresses absolute motion. Timestamps express Xorg's millisecond event
clock in microseconds.

Version 1 `xdg_system_bell_v1` rings the requesting client's XKB core
keyboard bell using its configured volume and audible-bell setting. Requests
can associate the XKB bell notification with a native toplevel window or ring
without a window. Requests with surfaces that have other roles are ignored.

### Outputs

Version 4 `wl_output` describes each connected, enabled RandR desktop output.
It reports geometry, physical size, the current mode and refresh rate, rotation
and reflection, subpixel order, a stable name, and a description. Output changes
and disconnects update existing clients. Screens without RandR output data use
the native X screen geometry. Separate X screens expose separate output globals;
surface windows currently use the first X screen.

Version 3 `zxdg_output_manager_v1` exposes each output's logical position
and rotated size from the same RandR layout as `wl_output`. At scale 1,
logical dimensions match the output's desktop rectangle. Names and
descriptions match `wl_output`. Version 3 updates use `wl_output.done`
when the core binding supports it. Older bindings use `zxdg_output_v1.done`.

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

X11 `CLIPBOARD` ownership is bridged in both directions through server APIs.
The latest authenticated Wayland selection owns the global X11 clipboard;
foreign X11 selections are offered to every Wayland seat. Selection conversion
supports `TARGETS`, `TIMESTAMP`, `UTF8_STRING`, `TEXT`, and MIME-named targets.
Legacy X11 `STRING` imports are converted from Latin-1 to UTF-8. Transfers use
nonblocking pipes and the ICCCM `INCR` handshake, with 64 KiB buffers, up to 32
concurrent conversions, and a 10 second inactivity timeout. Slow readers apply
backpressure without blocking the server. Destroyed requestors, reader hangups,
owner disconnects, and resets clean up pending conversions. Selection ownership
and transfer windows are unmapped InputOnly windows. X11 `MULTIPLE` conversions
and legacy `STRING` export are not implemented yet.

`zwp_primary_selection_device_manager_v1` supports primary selection between
Wayland clients and bridges it to X11 `PRIMARY` in both directions. Primary
selection has independent ownership and serial tracking on each seat.
Only the keyboard-focused client receives a live offer. On focus loss it
receives a null selection and its previous offer becomes inactive. X11
`PRIMARY` reuses clipboard format negotiation and asynchronous INCR transfers.

Xdnd versions 3 through 5 bridge pointer drags in both directions. Mapped xdg
toplevels advertise `XdndAware`. Outgoing drags discover aware ancestors and
validated proxy windows, publish type and action lists, and coalesce pointer
motion while awaiting status. Drop waits for the latest status reply; selection
ownership remains available until completion. Copy, move, and ask actions map
to Wayland drag actions, including the final action after an ask drop. Incoming
drags use the X11 source's client pointer to choose a seat, retain its pointer
grab, and use committed surface-tree hit testing. X11 source actions and target
acceptance update status replies. Owner or target destruction, selection loss, failed transfers, and server reset clean
up drag state. Status replies time out after 10 seconds and drop completion
after 30 seconds. Because `XdndSelection` is global, one X11 bridge drag can be
active at a time; another Wayland drag cannot replace a pending X11 drop.
Xdnd versions 1 and 2 are not implemented.

## Still to port

- Additional DMA-BUF formats.
- Follow the X11 desktop's integer scale via XSETTINGS (`Gdk/WindowScalingFactor`).
- Leases.
- Text-input-v3 is deferred.
- Server-side decoration negotiation.

## Test inventory

Every check runs against the isolated module test host.

- `native-sdk` loads a probe for the required server exports and checks an X11
  client connection.
- `native-lifecycle` loads the actual module and checks resets, socket
  cleanup, late and recreated runtime directories, recovery after native
  bridge closure, failed startup, and conflicting listeners.
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
- `native-shm` checks 32-bit and 16-bit imports, wide and packed strides,
  buffer release, pixel readback, and redraws after the client reuses its storage.
- `native-dmabuf` checks modifier advertisements, both buffer creation
  requests, rejected parameters, retained buffers and synchronized
  subsurfaces, reuse after release, and Y inversion with every buffer
  transform. It also checks the absence of the global without DRI3, default
  and per-surface feedback, sealed format tables, asynchronous device
  discovery, and version 3 fallback. The Xvfb driver uses guarded memory-backed
  pixmaps, rejects direct readback of test imports, and uses an eventfd to
  exercise asynchronous release and destruction while a fence is pending.
- `native-shm-fault` truncates an imported file during rendering to exercise
  Spaghetti's SIGBUS handling.
- `native-xdg` checks mapped pixel content, a test X11 window manager, configure
  serials, WM messages and properties, size hints, parent inheritance, protocol
  errors, remapping, and resets.
- `native-popup` checks positioner constraints, pixel readback, parent movement,
  nested placement and dismissal, popup reposition tokens and commit ordering,
  reactive placement, protocol versions, serial validation, pointer and
  keyboard grabs, focus restoration, and grab cleanup on disconnect.
- `native-present` checks copied pixels and matches Wayland frame callback
  timestamps to X11 Present pixmap completion events. It checks synchronized
  and asynchronous timing, reset to vsync, and duplicate tearing controls.
- `native-transforms` checks all buffer orientations, integer scales, committed
  geometry, retained pixels after release, and invalid surface state.
- `native-viewport` checks cropped pixels across buffer orientations and scales,
  fractional rectangles, commit timing, protocol errors, destruction, and resets.
- `native-single-pixel` checks buffer reuse, viewport resizing after buffer
  destruction, premultiplied blending, and synchronized subsurface updates.
- `native-shadows` checks desktop pixels around client shadow margins, preserved
  alpha, compositor ownership changes, resize size hints, geometry commits,
  remapping, and resets.
- `native-geometry` checks that input-driven repaints keep native geometry
  stable and only changed shapes and hints emit notifications. It also checks
  cropped input coordinates, visible window placement, status subsurfaces
  extending past windowed, maximized, and fullscreen window edges, delayed
  configure replies, and compositor changes while a resize is pending.
- `native-subsurfaces` checks synchronized content, nested commit dependencies,
  parent-committed position and stacking, alpha, negative coordinates, input
  regions, buffer transforms and viewports, role lifetime, destruction, and reset.
- `native-keymap` parses sealed keymap descriptors and checks live layout
  changes.
- `native-input` injects XTEST pointer and keyboard events, exercises focus and
  implicit grabs, and checks modifiers, repeat controls, client isolation,
  subsurface destruction, and master-device hotplug.
- `native-scroll` sends XI2 touchpad valuators and checks fractional deltas,
  diagonal scroll frames, emulated button suppression, and discrete wheels.
- `native-constraints` checks X11 cursor positions, lock motion suppression,
  button delivery, nonrectangular and double-buffered regions, synchronized
  subsurface commits, focus and lifetime rules, duplicate requests, window
  manager handoff, and disconnect cleanup.
- `native-relative-pointer` checks processed and raw delta delivery during
  normal motion, locks, and confinement, and motion-only pointer frames
  during a lock. It also covers absolute motion,
  scroll and foreign-grab filtering, client and seat isolation, resource
  lifetime, and master-device removal.
- `native-system-bell` checks XKB bell notifications with and without a
  toplevel window and checks that roleless surfaces are ignored.
- `native-cursor` reads back ARGB cursor images through XFixes.
- `native-interactive` checks move/resize serials, all resize directions, and
  pointer-grab transfer to a test WM.
- `native-outputs` checks output protocol versions, current modes, all RandR
  rotation/reflection combinations, physical metadata, disconnect/reconnect,
  non-desktop outputs, and multiple X screens. It also checks xdg-output
  logical geometry, metadata, update ordering, and binding lifetimes.
- `native-output-surfaces` checks membership during movement, minimize/restore,
  synchronized child commits, cursor updates, output binding/removal, and server
  reset.
- `native-data` checks clipboard focus and serial validation, source
  replacement, MIME offers, large and blocked transfers, protocol versions,
  disconnects, and reset cleanup.
- `native-selections` checks X11 clipboard ownership, text and binary MIME
  negotiation, timestamps, unsupported targets, small and large transfers in
  both directions, INCR backpressure, omitted size hints, large property chunks,
  blocked readers, Latin-1 imports, ownership replacement, owner disconnects, and resets.
  It also checks Wayland primary selection, both X11 `PRIMARY` transfer
  directions, focus loss, serial ownership, and clipboard isolation.
- `native-xdnd` checks both drag directions for versions 3 through 5, type
  lists, validated proxies, copy/move/ask actions, final ask actions, selection
  transfers before and after drop, coalesced motion, drops awaiting status,
  overlapping drags, clipboard isolation, rejected drops, destroyed targets,
  disconnected sources, selection loss, retained X11 pointer grabs, and resets.
  It also checks late Wayland action replies after crossing into an X11 target.
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
`MbDataBridge` observes local selection changes, seat retirement, and source
destruction. `native_selection` shares its transfer engine between `CLIPBOARD`
and `XdndSelection`. Imported sources retain a MIME-to-atom mapping and a
conversion timestamp; their provider notifications can drive drag negotiation.
Foreign window watches remain selected while either a drag or an active
transfer refers to that window. Releasing one selection does not clear another
client's newer ownership. INCR imports accept omitted advisory sizes, including
those used by xclip. A manual xclip smoke check covers small and roughly 1.5 MB
transfers in both directions. The matching Spaghetti SDK already provides the
selection and property APIs used by this bridge; no server ABI change is needed.
`mb_data_import_selection` publishes a foreign selection to active seats and
newly created data devices. These are private module interfaces; configuration
ABI 2.0 and the Xorg extension ABI remain unchanged.
