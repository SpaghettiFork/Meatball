/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "server.h"

#include <X11/Xatom.h>
#include <property.h>
#include <resource.h>
#include <string.h>
#include <window.h>

#define WINDOW_TRACKER_NAME "MeatballWindow"

static int
window_deleted (void *data, XID id)
{
    MbWindow *window = data;
    MbClient *owner = mb_client_lookup (window->owner);

    window->id = None;
    if (owner && !window->destroying)
        mb_client_disconnect (owner);
    return Success;
}

Bool
mb_windows_initialize (void)
{
    struct
    {
        const char *name;
        Atom *atom;
    } atoms[] = {
        { "WM_PROTOCOLS", &compositor.atoms.wm_protocols },
        { "WM_DELETE_WINDOW", &compositor.atoms.wm_delete_window },
        { "_NET_WM_NAME", &compositor.atoms.net_wm_name },
        { "UTF8_STRING", &compositor.atoms.utf8_string },
        { "_NET_WM_PID", &compositor.atoms.net_wm_pid },
    };
    unsigned int i;

    compositor.window_type =
        CreateNewResourceType (window_deleted, WINDOW_TRACKER_NAME);
    if (!compositor.window_type)
        return FALSE;
    for (i = 0; i < ARRAY_SIZE (atoms); i++)
    {
        *atoms[i].atom =
            MakeAtom (atoms[i].name, strlen (atoms[i].name), TRUE);
        if (!*atoms[i].atom)
            return FALSE;
    }
    return TRUE;
}

Bool
mb_window_create (MbWindow *window, MbClient *owner, ScreenPtr screen)
{
    WindowPtr native;
    XID id;
    XID values[] = { screen->blackPixel,
                     ExposureMask | StructureNotifyMask | PropertyChangeMask };
    CARD32 protocols[] = { compositor.atoms.wm_delete_window };
    LocalClientCredRec *credentials;
    CARD32 pid;
    int error;
    Mask event_mask;

    if (!owner || owner->failed || !owner->native || window->id)
        return FALSE;
    id = FakeClientID (owner->native->index);
    if (!LegalNewID (id, owner->native))
        return FALSE;
    native = CreateWindow (id, screen->root, 0, 0, MB_INITIAL_WINDOW_SIZE,
                           MB_INITIAL_WINDOW_SIZE, 0, InputOutput,
                           CWBackPixel | CWEventMask, values, CopyFromParent,
                           owner->native, CopyFromParent, &error);
    if (!native)
        return FALSE;
    event_mask = native->eventMask;
    native->eventMask = NoEventMask;
    if (!AddResource (id, RT_WINDOW, native))
        return FALSE;
    native->eventMask = event_mask;
    window->id = id;
    window->owner = owner->lifetime;
    window->screen = screen;
    /* The tracker uses the same XID, so FreeResource invalidates this handle
     * when a window or its native client is destroyed.
     */
    window->destroying = TRUE;
    if (!AddResource (id, compositor.window_type, window))
        goto fail;
    if (dixChangeWindowProperty (
            owner->native, native, compositor.atoms.wm_protocols, XA_ATOM,
            MB_PROPERTY_FORMAT_32, PropModeReplace, ARRAY_SIZE (protocols),
            protocols, TRUE) != Success)
        goto fail;
    if (GetLocalClientCreds (owner->native, &credentials) != Success)
        goto fail;
    pid = credentials->pid;
    FreeLocalClientCreds (credentials);
    if (dixChangeWindowProperty (
            owner->native, native, compositor.atoms.net_wm_pid, XA_CARDINAL,
            MB_PROPERTY_FORMAT_32, PropModeReplace, 1, &pid, TRUE) != Success)
        goto fail;
    window->destroying = FALSE;
    return TRUE;

fail:
    FreeResource (id, RT_NONE);
    window->id = None;
    window->destroying = FALSE;
    return FALSE;
}

WindowPtr
mb_window_get (MbWindow *window, Mask access)
{
    WindowPtr native;
    MbClient *owner = mb_client_lookup (window->owner);

    if (!window->id || !owner ||
        dixLookupWindow (&native, window->id, owner->native, access) !=
            Success)
        return NULL;
    return native;
}

void
mb_window_destroy (MbWindow *window)
{
    if (window->id)
    {
        window->destroying = TRUE;
        FreeResource (window->id, RT_NONE);
        window->id = None;
        window->destroying = FALSE;
    }
}
