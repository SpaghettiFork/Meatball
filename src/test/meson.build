sdk_probe = shared_module('meatball-sdk-probe', 'sdk_probe.c',
                          dependencies: server_sdk,
                          install: false)

if spaghetti_build_dir != ''
    host_hook = static_library('meatball-host-hook', 'host.c',
                               dependencies: server_sdk)
    host_inputs = run_command(python, 'sdk_paths.py', spaghetti_build_dir,
                               'inputs', check: true).stdout().strip().split('\n')
    host_link_args = get_option('c_link_args')
    if get_option('b_sanitize') != 'none'
        host_link_args += '-fsanitize=' + get_option('b_sanitize')
    endif
    test_host = custom_target('meatball-test-host',
                              input: host_hook,
                              output: 'meatball-test-host',
                              depend_files: host_inputs + files('sdk_paths.py'),
                              command: [python, '-B', files('build_host.py'),
                                        spaghetti_build_dir, '@INPUT@', '@OUTPUT@'] + host_link_args)
    test('native-sdk', python,
         args: ['-B', files('host_smoke.py'), test_host, sdk_probe],
         timeout: 30)
    test('native-lifecycle', python,
         args: ['-B', files('native_lifecycle.py'), test_host, sauce],
         timeout: 60)
    keymap_probe = shared_module('meatball-keymap-probe', ['probe.c', 'keymap_probe.c'],
                                 dependencies: [common_dep, meatball_headers],
                                 c_args: ['-UNDEBUG'],
                                 link_with: meatball)
    test('native-keymap', python,
         args: ['-B', files('native_keymap.py'), test_host, keymap_probe],
         timeout: 30)
    test('native-input', python,
         args: ['-B', files('native_input.py'), test_host, sauce],
         timeout: 60)
    test('native-cursor', python,
         args: ['-B', files('native_cursor.py'), test_host, sauce],
         timeout: 60)
    test('native-interactive', python,
         args: ['-B', files('native_interactive.py'), test_host, sauce],
         timeout: 60)
    output_probe = shared_module('meatball-output-probe', ['probe.c', 'output_probe.c'],
                                 dependencies: [common_dep, meatball_headers],
                                 c_args: ['-UNDEBUG'],
                                 link_with: meatball)
    test('native-outputs', python,
         args: ['-B', files('native_outputs.py'), test_host, sauce, output_probe],
         timeout: 60)
    test('native-output-surfaces', python,
         args: ['-B', files('native_output_surfaces.py'), test_host, output_probe],
         timeout: 60)
    test('native-data', python,
         args: ['-B', files('native_data.py'), test_host, sauce],
         timeout: 60)
    test('native-drag', python,
         args: ['-B', files('native_drag.py'), test_host, sauce],
         timeout: 60)
    test('native-loop', python,
         args: ['-B', files('native_loop.py'), test_host, sauce],
         timeout: 30)
    test('native-surfaces', python,
         args: ['-B', files('native_surfaces.py'), test_host, sauce],
         timeout: 30)
    test('native-subsurfaces', python,
         args: ['-B', files('native_subsurfaces.py'), test_host, sauce],
         timeout: 60)
    test('native-shm', python,
         args: ['-B', files('native_shm.py'), test_host, sauce],
         timeout: 30)
    test('native-xdg', python,
         args: ['-B', files('native_xdg.py'), test_host, sauce],
         timeout: 30)
    test('native-geometry', python,
         args: ['-B', files('native_geometry.py'), test_host, sauce],
         timeout: 30)
    test('native-shadows', python,
         args: ['-B', files('native_shadows.py'), test_host, sauce],
         timeout: 30)
    test('native-transforms', python,
         args: ['-B', files('native_transforms.py'), test_host, sauce],
         timeout: 30)
    test('native-viewport', python,
         args: ['-B', files('native_viewport.py'), test_host, sauce],
         timeout: 30)
    loop_probe = shared_module('meatball-loop-probe', ['probe.c', 'loop_probe.c'],
                               dependencies: [common_dep, meatball_headers],
                               c_args: ['-UNDEBUG'],
                               link_with: meatball)
    test('native-scheduler', python,
         args: ['-B', files('native_scheduler.py'), test_host, loop_probe],
         timeout: 30)
    client_probe = shared_module('meatball-client-probe', ['probe.c', 'client_probe.c'],
                                 dependencies: [common_dep, meatball_headers],
                                 c_args: ['-UNDEBUG'],
                                 link_with: meatball)
    test('native-clients', python,
         args: ['-B', files('native_clients.py'), test_host, client_probe],
         timeout: 30)
    socket_probe = shared_module('meatball-socket-probe', ['probe.c', 'socket_probe.c'],
                                 dependencies: [common_dep, meatball_headers],
                                 c_args: ['-UNDEBUG'],
                                 link_args: ['-Wl,--wrap=chmod', '-Wl,--wrap=fchownat'],
                                 link_with: meatball)
    test('native-socket', python,
         args: ['-B', files('native_socket.py'), test_host, socket_probe],
         timeout: 30)
    shm_fault_probe = shared_module('meatball-shm-fault-probe', ['probe.c', 'shm_fault_probe.c'],
                                    dependencies: [common_dep, meatball_headers],
                                    c_args: ['-UNDEBUG'],
                                    link_args: ['-Wl,--wrap=shm_create_pixmap_from_fd'],
                                    link_with: meatball)
    test('native-shm-fault', python,
         args: ['-B', files('native_shm.py'), test_host, shm_fault_probe, '--fault'],
         timeout: 30)
else
    message('Set spaghetti_build_dir to enable the isolated module test host')
endif
