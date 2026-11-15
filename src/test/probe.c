/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "probe.h"

#include <assert.h>
#include <extension.h>
#include <extnsionst.h>
#include <stdlib.h>
#include <xf86Module.h>

#define PROBE_EXTENSION "MEATBALL_TEST"
#define SOCKET_PATH_ENV "MEATBALL_TEST_SOCKET"

static Bool start_pending;

static void
probe_wakeup (void *data, int result)
{
}

static void
probe_start (void *data, void *timeout)
{
    struct meatball_config config = {
        .socket_path = getenv (SOCKET_PATH_ENV),
        .socket_mode = MEATBALL_DEFAULT_SOCKET_MODE,
    };

    RemoveBlockAndWakeupHandlers (probe_start, probe_wakeup, NULL);
    start_pending = FALSE;
    assert (meatball_initialize (&config));
    mb_probe_start ();
}

static void
probe_reset (ExtensionEntry *extension)
{
    if (start_pending)
        RemoveBlockAndWakeupHandlers (probe_start, probe_wakeup, NULL);
    start_pending = FALSE;
    mb_probe_stop ();
    meatball_shutdown ();
}

static int
probe_request (ClientPtr client)
{
    return BadRequest;
}

static void
probe_initialize (void)
{
    assert (AddExtension (PROBE_EXTENSION, 0, 0, probe_request, probe_request,
                          probe_reset, StandardMinorOpcode));
    start_pending =
        RegisterBlockAndWakeupHandlers (probe_start, probe_wakeup, NULL);
    assert (start_pending);
}

static void *
probe_setup (void *module, void *opts, int *errmaj, int *errmin)
{
    static const ExtensionModule extension = {
        .initFunc = probe_initialize,
        .name = PROBE_EXTENSION,
    };

    LoadExtensionList (&extension, 1, FALSE);
    return module;
}

static XF86ModuleVersionInfo version = {
    .modname = "meatball-test-probe",
    .vendor = "Spaghetti Fork",
    ._modinfo1_ = MODINFOSTRING1,
    ._modinfo2_ = MODINFOSTRING2,
    .xf86version = XORG_VERSION_CURRENT,
    .abiclass = ABI_CLASS_EXTENSION,
    .abiversion = ABI_EXTENSION_VERSION,
};

_X_EXPORT XF86ModuleData sauceModuleData = {
    .vers = &version,
    .setup = probe_setup,
};
