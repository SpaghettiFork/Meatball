srcs_meatball = [
    'meatball.c',
    'meatball_log.c',
    'run.c',
    'native_client.c',
    'native_window.c',
    'native_surface.c',
]

meatball_headers = declare_dependency(include_directories: '.')

meatball = static_library('meatball',
    srcs_meatball,
    dependencies: [common_dep, meatball_headers],
)
