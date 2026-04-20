project('Meatball', 'c',
        default_options: [
            'buildtype=debugoptimized',
            'c_std=gnu99',
        ],
        version: '1.0.4',
        meson_version: '>= 0.58.0',
)

egl_renderer = get_option('egl_renderer')
module_dir = join_paths(get_option('libdir'), get_option('module_dir'))

cc = meson.get_compiler('c')

add_project_arguments('-fno-strict-aliasing', language : 'c')
add_project_arguments('-fvisibility=hidden', language : 'c')
add_project_arguments('-D_GNU_SOURCE', language: 'c')
add_project_arguments('-D_DEFAULT_SOURCE', language: 'c')
add_project_arguments('-DXCSECURITY', language: 'c')

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

libdrm_req = '>= 2.4.116'
libselinux_req = '>= 2.0.86'
xext_req = '>= 1.0.99.4'
xproto_req = '>= 7.0.31'
wayland_req = '>= 1.21.0'
gbm_req = '>= 10.2'
xf86dgaproto_req = '>= 2.0.99.1'
xshmfence_req = '>= 1.1'

x11_dep = dependency('x11')
xpresent_dep = dependency('xpresent', required : true)
xcb_randr_dep = dependency('xcb-randr', required : true)
xcb_dri3_dep = dependency('xcb-dri3', required : true)
xcb_shape_dep = dependency('xcb-shape', required : true)
xcb_shm_dep = dependency('xcb-shm', required : true)
x11_xcb_dep = dependency('x11-xcb', required : true)
xcb_dep = dependency('xcb', required : true)
xfixes_dep = dependency('xfixes', required : true)
xrandr_dep = dependency('xrandr', required : true)
xi_dep = dependency('xi', required : true)
xshmfence_dep = dependency('xshmfence', required : true)

pixman_dep = dependency('pixman-1')
libdrm_dep = dependency('libdrm', version: libdrm_req, required: true)
epoxy_dep = dependency('epoxy', required: true)

wayland_dep = dependency('wayland-server', version: wayland_req, required: true)

common_dep = [
    x11_dep,
    xpresent_dep,
    xcb_randr_dep,
    xcb_dri3_dep,
    xcb_shape_dep,
    xcb_shm_dep,
    x11_xcb_dep,
    xcb_dep,
    xfixes_dep,
    xrandr_dep,
    xi_dep,
    xshmfence_dep,

    pixman_dep,
    libdrm_dep,
    epoxy_dep,

    wayland_dep,
]

if cc.compiles('''
    #define _GNU_SOURCE 1
    #include <pthread.h>
    void foo(int bar) { pthread_setname_np(pthread_self(), "example"); }
''',
    args: '-Werror-implicit-function-declaration',
    name: 'pthread_setname_np(tid, name)')
    add_project_arguments('-DHAVE_PTHREAD_SETNAME_NP_WITH_TID=1', language: 'c')
elif cc.compiles('''
    #define _GNU_SOURCE 1
    #include <pthread.h>
    void foo(int bar) { pthread_setname_np("example"); }
''',
    args: '-Werror-implicit-function-declaration',
    name: 'pthread_setname_np(name)')
    add_project_arguments('-DHAVE_PTHREAD_SETNAME_NP_WITHOUT_TID=1', language: 'c')
endif

if egl_renderer
    add_project_arguments('-DHAVE_EGL_SUPPORT=1', language: 'c')
endif

subdir('meatball')

meatball_inc = include_directories(
    'meatball'
)

srcs_sauce = [
    'sauce.c'
]

shared_module('sauce',
    srcs_sauce,
    include_directories: [
        meatball_inc
    ],
    dependencies: [
        common_dep,
        meatball_headers
    ],
    link_with: [
        meatball
    ],
    install: true,
    install_dir: module_dir,
)