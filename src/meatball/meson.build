scanner = find_program('wayland-scanner', native: true)
xdg_header = custom_target('native-xdg-header',
                           input: 'wayland-proto/xdg-shell.xml', output: 'xdg-shell.h',
                           command: [scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
xdg_code = custom_target('native-xdg-code',
                         input: 'wayland-proto/xdg-shell.xml', output: 'xdg-shell.c',
                         command: [scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
viewport_header = custom_target('native-viewport-header',
                                input: 'wayland-proto/viewporter.xml', output: 'viewporter.h',
                                command: [scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
viewport_code = custom_target('native-viewport-code',
                              input: 'wayland-proto/viewporter.xml', output: 'viewporter.c',
                              command: [scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

srcs_meatball = [
    'meatball.c',
    'meatball_log.c',
    'run.c',
    'native_client.c',
    'native_keymap.c',
    'native_window.c',
    'native_surface.c',
    'native_subsurface.c',
    'native_shm.c',
    'native_render.c',
    'native_xdg.c',
    'native_viewport.c',
    xdg_header,
    xdg_code,
    viewport_header,
    viewport_code,
]

meatball_headers = declare_dependency(include_directories: '.')

meatball = static_library('meatball',
    srcs_meatball,
    dependencies: [common_dep, meatball_headers],
)
