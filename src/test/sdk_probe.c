/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <xorg-server.h>

#include <dixstruct.h>
#include <dri3.h>
#include <external.h>
#include <present.h>
#include <shmint.h>
#include <xace.h>
#include <xf86Module.h>

__attribute__ ((
    used)) static __typeof__ (&dixCreateExternalClient) create_client =
    dixCreateExternalClient;
__attribute__ ((used)) static __typeof__ (&present_pixmap) present_buffer =
    present_pixmap;
__attribute__ ((
    used)) static __typeof__ (&shm_create_pixmap_from_fd) import_shm =
    shm_create_pixmap_from_fd;
__attribute__ ((used)) static __typeof__ (&RRLeaseCreate) create_lease =
    RRLeaseCreate;
__attribute__ ((used)) static __typeof__ (&RRTerminateLease) terminate_lease =
    RRTerminateLease;
#ifdef XACE
__attribute__ ((
    used)) static __typeof__ (&XaceHookSendAccess) check_send_access =
    XaceHookSendAccess;
#endif
#ifdef DRI3
__attribute__ ((used)) static __typeof__ (&dri3_open) open_device = dri3_open;
__attribute__ ((
    used)) static __typeof__ (&dri3_get_supported_modifiers) query_modifiers =
    dri3_get_supported_modifiers;
#endif

static MODULESETUPPROTO (probe_setup);

static XF86ModuleVersionInfo version = {
    .modname = "meatball-sdk-probe",
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

static void *
probe_setup (void *module, void *opts, int *errmaj, int *errmin)
{
    return module;
}
