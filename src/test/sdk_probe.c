/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <xorg-server.h>

#include <dix.h>
#include <dixstruct.h>
#include <dri3.h>
#include <exevents.h>
#include <external.h>
#include <present.h>
#include <randrstr.h>
#include <shmint.h>
#include <xace.h>
#include <xf86Module.h>
#include <xkbsrv.h>

__attribute__ ((
    used)) static __typeof__ (&dixCreateExternalClient) create_client =
    dixCreateExternalClient;
__attribute__ ((used)) static __typeof__ (&present_pixmap) present_buffer =
    present_pixmap;
__attribute__ ((
    used)) static __typeof__ (&present_notify_msc) present_refresh =
    present_notify_msc;
__attribute__ ((
    used)) static __typeof__ (&present_select_input) present_events =
    present_select_input;
__attribute__ ((
    used)) static __typeof__ (&shm_create_pixmap_from_fd) import_shm =
    shm_create_pixmap_from_fd;
__attribute__ ((used)) static __typeof__ (&RRLeaseCreate) create_lease =
    RRLeaseCreate;
__attribute__ ((used)) static __typeof__ (&RRTerminateLease) terminate_lease =
    RRTerminateLease;
__attribute__ ((used)) static __typeof__ (&XISetEventMask) select_input =
    XISetEventMask;
__attribute__ ((used)) static RESTYPE *keyboard_subscription_type =
    &RT_XKBCLIENT;
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

__attribute__ ((used)) static DevPrivateKey output_private_key = &rrPrivKeyRec;
__attribute__ ((used)) static __typeof__ (&RRCrtcGetScanoutSize) output_size =
    RRCrtcGetScanoutSize;

__attribute__ ((used)) static __typeof__ (&GrabDevice) grab_device =
    GrabDevice;
__attribute__ ((used)) static __typeof__ (&WindowHasNewCursor) update_cursor =
    WindowHasNewCursor;

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
