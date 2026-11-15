/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <xorg-server.h>

#include <dlfcn.h>
#include <extension.h>
#include <stdlib.h>
#include <string.h>
#include <xf86Module.h>
#include <xf86Optionstr.h>

#define MODULE_PATH_ENV "MEATBALL_TEST_MODULE"
#define MODULE_DATA_SYMBOL "sauceModuleData"
#define SOCKET_PATH_ENV "MEATBALL_TEST_SOCKET"
#define SOCKET_MODE_ENV "MEATBALL_TEST_SOCKET_MODE"

extern void __real_InitExtensions (int argc, char **argv);
extern void __wrap_InitExtensions (int argc, char **argv);

void
__wrap_InitExtensions (int argc, char **argv)
{
    static void *module;

    if (!module)
    {
        const char *path = getenv (MODULE_PATH_ENV);
        XF86ModuleData *data;
        XF86OptionRec mode = { .opt_name = "SocketMode",
                               .opt_val = getenv (SOCKET_MODE_ENV) };
        XF86OptionRec path_option = {
            .opt_name = "SocketPath",
            .opt_val = getenv (SOCKET_PATH_ENV),
            .list.next = mode.opt_val ? &mode : NULL,
        };
        int major = 0, minor = 0;

        if (!path)
            FatalError ("meatball-test: %s is required\n", MODULE_PATH_ENV);
        module = dlopen (path, RTLD_NOW | RTLD_GLOBAL);
        if (!module)
            FatalError ("meatball-test: %s\n", dlerror ());
        data = dlsym (module, MODULE_DATA_SYMBOL);
        if (!data || !data->vers || !data->setup ||
            strcmp (data->vers->abiclass, ABI_CLASS_EXTENSION) ||
            data->vers->abiversion != ABI_EXTENSION_VERSION ||
            data->vers->xf86version != XORG_VERSION_CURRENT)
            FatalError ("meatball-test: incompatible module metadata\n");
        if (!data->setup (module, path_option.opt_val ? &path_option : NULL,
                          &major, &minor))
            FatalError ("meatball-test: module setup failed (%d, %d)\n", major,
                        minor);
    }

    __real_InitExtensions (argc, argv);
}
