build_version = vcs_tag(
    input: 'build_version.h.in',
    output: 'build_version.h',
    fallback: meson.project_version(),
    command: ['git', '-C', meson.project_source_root(),
              'describe', '--always', '--dirty', '--exclude=*'],
)

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

primary_xml = 'wayland-proto/primary-selection-unstable-v1.xml'
primary_header = custom_target('native-primary-header',
                               input: primary_xml, output: 'primary-selection.h',
                               command: [scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
primary_code = custom_target('native-primary-code',
                             input: primary_xml, output: 'primary-selection.c',
                             command: [scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

dmabuf_xml = 'wayland-proto/linux-dmabuf-unstable-v1.xml'
dmabuf_header = custom_target('native-dmabuf-header',
                              input: dmabuf_xml, output: 'linux-dmabuf.h',
                              command: [scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
dmabuf_code = custom_target('native-dmabuf-code',
                            input: dmabuf_xml, output: 'linux-dmabuf.c',
                            command: [scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

constraints_xml = 'wayland-proto/pointer-constraints-unstable-v1.xml'
constraints_header = custom_target('native-constraints-header',
                                   input: constraints_xml, output: 'pointer-constraints.h',
                                   command: [scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
constraints_code = custom_target('native-constraints-code',
                                 input: constraints_xml, output: 'pointer-constraints.c',
                                 command: [scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

relative_pointer_xml = 'wayland-proto/relative-pointer-unstable-v1.xml'
relative_pointer_header = custom_target('native-relative-pointer-header',
                                      input: relative_pointer_xml, output: 'relative-pointer.h',
                                      command: [scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
relative_pointer_code = custom_target('native-relative-pointer-code',
                                    input: relative_pointer_xml, output: 'relative-pointer.c',
                                    command: [scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

tearing_control_xml = 'wayland-proto/tearing-control-v1.xml'
tearing_control_header = custom_target('native-tearing-control-header',
                                      input: tearing_control_xml, output: 'tearing-control.h',
                                      command: [scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
tearing_control_code = custom_target('native-tearing-control-code',
                                    input: tearing_control_xml, output: 'tearing-control.c',
                                    command: [scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

system_bell_xml = 'wayland-proto/xdg-system-bell-v1.xml'
system_bell_header = custom_target('native-system-bell-header',
                                   input: system_bell_xml, output: 'xdg-system-bell.h',
                                   command: [scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
system_bell_code = custom_target('native-system-bell-code',
                                 input: system_bell_xml, output: 'xdg-system-bell.c',
                                 command: [scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

single_pixel_xml = 'wayland-proto/single-pixel-buffer-v1.xml'
single_pixel_header = custom_target('native-single-pixel-header',
                                    input: single_pixel_xml, output: 'single-pixel-buffer.h',
                                    command: [scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
single_pixel_code = custom_target('native-single-pixel-code',
                                  input: single_pixel_xml, output: 'single-pixel-buffer.c',
                                  command: [scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

srcs_meatball = [
    'meatball.c',
    build_version,
    'meatball_log.c',
    'run.c',
    'native_client.c',
    'native_keymap.c',
    'native_input.c',
    'native_data.c',
    'native_selection.c',
    'native_xdnd.c',
    'native_output.c',
    'native_cursor.c',
    'native_window.c',
    'native_surface.c',
    'native_subsurface.c',
    'native_buffer.c',
    'native_dmabuf.c',
    'native_shm.c',
    'native_render.c',
    'native_xdg.c',
    'native_viewport.c',
    xdg_header,
    xdg_code,
    viewport_header,
    viewport_code,
    primary_header,
    primary_code,
    dmabuf_header,
    dmabuf_code,
    constraints_header,
    constraints_code,
    relative_pointer_header,
    relative_pointer_code,
    tearing_control_header,
    tearing_control_code,
    system_bell_header,
    system_bell_code,
    single_pixel_header,
    single_pixel_code,
]

meatball_headers = declare_dependency(include_directories: '.')

meatball = static_library('meatball',
    srcs_meatball,
    dependencies: [common_dep, meatball_headers],
)
