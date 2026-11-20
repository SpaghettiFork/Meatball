/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "server.h"

#include <X11/Xatom.h>
#include <colormap.h>
#include <picturestr.h>
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
    static unsigned long tracker_generation;
    static RESTYPE tracker_type;
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
        { "_NET_WM_STATE", &compositor.atoms.net_wm_state },
        { "_NET_WM_MOVERESIZE", &compositor.atoms.net_wm_moveresize },
        { "_NET_WM_STATE_FULLSCREEN", &compositor.atoms.net_wm_fullscreen },
        { "_NET_WM_STATE_MAXIMIZED_HORZ",
          &compositor.atoms.net_wm_maximized_horz },
        { "_NET_WM_STATE_MAXIMIZED_VERT",
          &compositor.atoms.net_wm_maximized_vert },
        { "_NET_WM_STATE_FOCUSED", &compositor.atoms.net_wm_focused },
        { "WM_CHANGE_STATE", &compositor.atoms.wm_change_state },
        { "_GTK_FRAME_EXTENTS", &compositor.atoms.gtk_frame_extents },
        { "_MOTIF_WM_HINTS", &compositor.atoms.motif_wm_hints },
    };
    unsigned int i;

    if (tracker_generation != serverGeneration || !tracker_type)
    {
        tracker_type =
            CreateNewResourceType (window_deleted, WINDOW_TRACKER_NAME);
        if (!tracker_type)
            return FALSE;
        tracker_generation = serverGeneration;
    }
    compositor.window_type = tracker_type;
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
    XID values[] = { 0, 0,
                     ExposureMask | StructureNotifyMask | PropertyChangeMask,
                     0 };
    Mask attributes = CWBackPixel | CWBorderPixel | CWEventMask;
    VisualID visual_id = CopyFromParent;
    unsigned int depth = CopyFromParent;
    int i, j, k;
    CARD32 protocols[] = { compositor.atoms.wm_delete_window };
    LocalClientCredRec *credentials;
    CARD32 pid;
    int error;
    Mask event_mask;

    if (!owner || owner->failed || !owner->native || window->id)
        return FALSE;
    window->owner = owner->lifetime;
    window->screen = screen;
    for (i = 0; i < screen->numDepths && visual_id == CopyFromParent; i++)
    {
        DepthPtr allowed = &screen->allowedDepths[i];

        if (allowed->depth != MB_SHM_ARGB_DEPTH)
            continue;
        for (j = 0; j < allowed->numVids && visual_id == CopyFromParent; j++)
            for (k = 0; k < screen->numVisuals; k++)
            {
                VisualPtr visual = &screen->visuals[k];
                PictFormatPtr format;
                ColormapPtr colormap;
                XID colormap_id;

                if (visual->vid != allowed->vids[j])
                    continue;
                format = PictureMatchVisual (screen, allowed->depth, visual);
                if (!format || !format->direct.alphaMask)
                    continue;
                colormap_id = FakeClientID (owner->native->index);
                if (CreateColormap (colormap_id, screen, visual, &colormap,
                                    AllocNone,
                                    owner->native->index) != Success)
                    return FALSE;
                window->colormap = colormap_id;
                values[3] = colormap_id;
                attributes |= CWColormap;
                visual_id = visual->vid;
                depth = allowed->depth;
                break;
            }
    }
    id = FakeClientID (owner->native->index);
    if (!LegalNewID (id, owner->native))
        goto fail;
    native = CreateWindow (id, screen->root, 0, 0, MB_INITIAL_WINDOW_SIZE,
                           MB_INITIAL_WINDOW_SIZE, 0, InputOutput, attributes,
                           values, depth, owner->native, visual_id, &error);
    if (!native)
        goto fail;
    event_mask = native->eventMask;
    native->eventMask = NoEventMask;
    if (!AddResource (id, RT_WINDOW, native))
        goto fail;
    native->eventMask = event_mask;
    window->id = id;
    /* The tracker uses the same XID, so FreeResource invalidates this handle
     * when a window or its native client is destroyed.
     */
    window->destroying = TRUE;
    if (!AddResource (id, compositor.window_type, window))
    {
        window->id = id;
        goto fail;
    }
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
    if (!mb_input_select_window (window, owner))
        goto fail;
    return TRUE;

fail:
    mb_window_destroy (window);
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
    MbClient *owner = mb_client_lookup (window->owner);
    ColormapPtr colormap;

    if (window->id)
    {
        window->destroying = TRUE;
        FreeResource (window->id, RT_NONE);
        window->id = None;
        window->destroying = FALSE;
    }
    if (window->colormap && owner &&
        dixLookupResourceByType ((void **)&colormap, window->colormap,
                                 RT_COLORMAP, owner->native,
                                 DixDestroyAccess) == Success)
        FreeResource (window->colormap, RT_NONE);
    window->colormap = None;
}
