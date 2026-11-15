headers_meatball = []

srcs_meatball = [
    'meatball_log.c',
    'meatball.c',
    'run.c',
    'alloc.c',
    'fns.c',
    'output.c',
    'compositor.c',
    'surface.c',
    'region.c',
    'shm.c',
    'atoms.c',
    'subcompositor.c',
    'positioner.c',
    'xdg_wm.c',
    'xdg_surface.c',
    'xdg_toplevel.c',
    'frame_clock.c',
    'xerror.c',
    'ewmh.c',
    'timer.c',
    'subsurface.c',
    'seat.c',
    'data_device.c',
    'xdg_popup.c',
    'dmabuf.c',
    'buffer.c',
    'select.c',
    'xdata.c',
    'xsettings.c',
    'dnd.c',
    'icon_surface.c',
    'primary_selection.c',
    'renderer.c',
    'explicit_synchronization.c',
    'transform.c',
    'wp_viewporter.c',
    'decoration.c',
    'text_input.c',
    'single_pixel_buffer.c',
    'drm_lease.c',
    'pointer_constraints.c',
    'time.c',
    'relative_pointer.c',
    'keyboard_shortcuts_inhibit.c',
    'idle_inhibit.c',
    'process.c',
    'fence_ring.c',
    'pointer_gestures.c',
    'test.c',
    'buffer_release.c',
    'xdg_activation.c',
    'tearing_control.c',
    'sync_source.c',
    'xdg_system_bell.c',
    'picture_renderer.c',
]

# We need the headers in order to actually get a build.
subdir('test')
subdir('wayland-proto')

meatball_headers = declare_dependency(
    sources : headers_meatball,
    include_directories: [
        '.',
        'static-headers',
        'gpu-shaders',
    ]
)

meatball = static_library('meatball',
    srcs_meatball,
    dependencies : [
        common_dep,
        meatball_headers
    ]
)
