scanner = find_program('wayland-scanner', native: true)
xdg_header = custom_target('native-xdg-header',
                           input: 'wayland-proto/xdg-shell.xml', output: 'xdg-shell.h',
                           command: [scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
xdg_code = custom_target('native-xdg-code',
                         input: 'wayland-proto/xdg-shell.xml', output: 'xdg-shell.c',
                         command: [scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

srcs_meatball = [
    'meatball.c',
    'meatball_log.c',
    'run.c',
    'native_client.c',
    'native_window.c',
    'native_surface.c',
    'native_shm.c',
    'native_render.c',
    'native_xdg.c',
    xdg_header,
    xdg_code,
]

meatball_headers = declare_dependency(include_directories: '.')

meatball = static_library('meatball',
    srcs_meatball,
    dependencies: [common_dep, meatball_headers],
)
