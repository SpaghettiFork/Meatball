sdk_probe = shared_module('meatball-sdk-probe', 'sdk_probe.c',
                          dependencies: server_sdk,
                          install: false)

if spaghetti_build_dir != ''
    host_hook = static_library('meatball-host-hook', 'host.c',
                               dependencies: server_sdk)
    host_inputs = run_command(python, 'sdk_paths.py', spaghetti_build_dir,
                               'inputs', check: true).stdout().strip().split('\n')
    test_host = custom_target('meatball-test-host',
                              input: host_hook,
                              output: 'meatball-test-host',
                              depend_files: host_inputs + files('sdk_paths.py'),
                              command: [python, '-B', files('build_host.py'),
                                        spaghetti_build_dir, '@INPUT@', '@OUTPUT@'])
    test('native-sdk', python,
         args: ['-B', files('host_smoke.py'), test_host, sdk_probe],
         timeout: 30)
    test('native-lifecycle', python,
         args: ['-B', files('native_lifecycle.py'), test_host, sauce],
         timeout: 60)
    test('native-loop', python,
         args: ['-B', files('native_loop.py'), test_host, sauce],
         timeout: 30)
    loop_probe = shared_module('meatball-loop-probe', 'loop_probe.c',
                               dependencies: [common_dep, meatball_headers],
                               c_args: ['-UNDEBUG'],
                               link_with: meatball)
    test('native-scheduler', python,
         args: ['-B', files('native_scheduler.py'), test_host, loop_probe],
         timeout: 30)
else
    message('Set spaghetti_build_dir to enable the isolated module test host')
endif
