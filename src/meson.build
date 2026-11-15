project('Meatball', 'c',
        default_options: [
            'buildtype=debugoptimized',
            'c_std=gnu99',
        ],
        version: '2.0.0',
        meson_version: '>= 0.58.0',
)

module_dir = join_paths(get_option('libdir'), get_option('module_dir'))

cc = meson.get_compiler('c')
python = import('python').find_installation('python3')
spaghetti_build_dir = get_option('spaghetti_build_dir')

if spaghetti_build_dir != ''
    spaghetti_build_dir = join_paths(meson.project_source_root(), spaghetti_build_dir)
    sdk_paths = run_command(python, 'test/sdk_paths.py',
                            spaghetti_build_dir, 'includes', check: false)
    if sdk_paths.returncode() != 0
        error(sdk_paths.stderr().strip())
    endif
    sdk_includes = []
    foreach path : sdk_paths.stdout().strip().split('\n')
        sdk_includes += include_directories(path)
    endforeach
    server_sdk = declare_dependency(include_directories: sdk_includes,
                                    dependencies: dependency('pixman-1'))
else
    server_sdk = dependency('xorg-server')
endif

if not cc.compiles('''
    #include <xorg-server.h>
    #include <dixstruct.h>
    #include <external.h>
    #include <present.h>
    #include <shmint.h>
    typedef PixmapPtr (*ShmImport)(ScreenPtr, int, CARD16, CARD16,
                                   CARD8, CARD8, CARD32, CARD32);
    _Static_assert(__builtin_types_compatible_p(
                       __typeof__(&shm_create_pixmap_from_fd), ShmImport),
                   "Spaghetti SHM import requires a 32-bit stride");
    int main(void) {
        ExternalClientCallbacks callbacks = {0};
        return sizeof(callbacks) == 0;
    }
''', dependencies: server_sdk, name: 'Spaghetti native extension SDK')
    error('Use the matching Spaghetti SDK or set spaghetti_build_dir to its built tree')
endif

add_project_arguments('-fno-strict-aliasing', language : 'c')
add_project_arguments('-fvisibility=hidden', language : 'c')
add_project_arguments('-D_GNU_SOURCE', language: 'c')
add_project_arguments('-D_DEFAULT_SOURCE', language: 'c')

add_project_link_arguments('-fvisibility=hidden', language : 'c')

if cc.get_id() == 'gcc' or cc.get_id() == 'clang'
    test_wflags = [
        '-Wall',
        '-Wpointer-arith',
        '-Wmissing-declarations',
        '-Wformat=2',
        '-Wstrict-prototypes',
        '-Wmissing-prototypes',
        '-Wnested-externs',
        '-Wbad-function-cast',
        '-Wold-style-definition',
        '-Wunused',
        '-Wuninitialized',
        '-Wshadow',
        '-Wmissing-noreturn',
        '-Wmissing-format-attribute',
        '-Wredundant-decls',
        '-Werror=implicit',
        '-Werror=nonnull',
        '-Werror=init-self',
        '-Werror=main',
        '-Werror=missing-braces',
        '-Werror=sequence-point',
        '-Werror=return-type',
        '-Werror=trigraphs',
        '-Werror=array-bounds',
        '-Werror=write-strings',
        '-Werror=address',
        '-Werror=int-to-pointer-cast',
        '-Werror=pointer-to-int-cast',
        '-Wvla',
    ]
else
    test_wflags = []
endif

common_wflags = []
foreach wflag: test_wflags
    if cc.has_argument(wflag)
        common_wflags += [ wflag ]
    endif
endforeach

add_project_arguments(common_wflags, language : ['c', 'objc'])

pixman_dep = dependency('pixman-1')
libdrm_dep = dependency('libdrm', version: '>= 2.4.116')
wayland_dep = dependency('wayland-server', version: '>= 1.21.0')

common_dep = [server_sdk, pixman_dep, libdrm_dep, wayland_dep]

subdir('meatball')

meatball_inc = include_directories(
    'meatball'
)

srcs_sauce = [
    'sauce.c'
]

sauce = shared_module('sauce',
    srcs_sauce,
    include_directories: [
        meatball_inc
    ],
    dependencies: [
        common_dep,
        meatball_headers,
        server_sdk,
    ],
    link_with: [
        meatball
    ],
    link_args: ['-Wl,-z,now'],
    install: true,
    install_dir: module_dir,
)

if get_option('tests')
    subdir('test')
endif
