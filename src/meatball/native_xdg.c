/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "server.h"
#include "xdg-shell.h"

#include <X11/Xatom.h>
#include <X11/extensions/shapeconst.h>
#include <inputstr.h>
#include <limits.h>
#include <property.h>
#include <propertyst.h>
#include <selection.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-protocol.h>
#include <window.h>
#include <xace.h>

#define XDG_VERSION 1
#define CONFIGURE_LIMIT 256
#define SEND_EVENT_MASK 0x80
#define WM_SOURCE_APPLICATION 1

enum WmMoveResize
{
    WM_RESIZE_TOP_LEFT,
    WM_RESIZE_TOP,
    WM_RESIZE_TOP_RIGHT,
    WM_RESIZE_RIGHT,
    WM_RESIZE_BOTTOM_RIGHT,
    WM_RESIZE_BOTTOM,
    WM_RESIZE_BOTTOM_LEFT,
    WM_RESIZE_LEFT,
    WM_MOVE,
};
#define WM_STATE_ADD 1
#define WM_STATE_REMOVE 0
#define WM_ICONIC_STATE 3
#define WM_SIZE_HINT_WORDS 18
#define WM_HINT_FLAGS 0
#define WM_HINT_MIN_WIDTH 5
#define WM_HINT_MIN_HEIGHT 6
#define WM_HINT_MAX_WIDTH 7
#define WM_HINT_MAX_HEIGHT 8
#define WM_HINT_WIDTH_INC 9
#define WM_HINT_HEIGHT_INC 10
#define WM_HINT_BASE_WIDTH 15
#define WM_HINT_BASE_HEIGHT 16
#define WM_HINT_MIN_SIZE (1u << 4)
#define WM_HINT_MAX_SIZE (1u << 5)
#define WM_HINT_RESIZE_INC (1u << 6)
#define WM_HINT_BASE_SIZE (1u << 8)
#define WM_RESIZE_INCREMENT 1
#define MAX_TOPLEVEL_STATES 3
#define WM_STATE_ATOM_LIMIT 128
#define MOTIF_HINT_WORDS 5
#define MOTIF_HINT_FLAGS 0
#define MOTIF_HINT_DECORATIONS (1u << 1)
#define COMPOSITOR_SELECTION_FORMAT "_NET_WM_CM_S%d"

struct XdgBase
{
    struct wl_resource *resource;
    unsigned int refs, children;
};

struct Configure
{
    struct wl_list link;
    uint32_t serial;
};

struct MbXdgSurface
{
    struct XdgBase *base;
    struct wl_resource *resource;
    struct wl_resource *toplevel;
    MbSurface *surface;
    unsigned int refs;
    struct wl_list configures;
    unsigned int configure_count;
    Bool initialized, configured, map_requested;
    int32_t last_width, last_height;
    int32_t min_width, min_height, max_width, max_height;
    Bool hints_changed;
    Bool geometry_set, frame_extents_set, anchor_set;
    int32_t anchor_x, anchor_y;
    int32_t geometry_x, geometry_y, geometry_width, geometry_height;
    XID parent;
    XID window_id;
    uint64_t owner;
    CARD32 frame_extents[4];
    Atom compositor_selection;
};

static void wm_message (struct MbXdgSurface *, Atom, CARD32, CARD32, CARD32,
                        CARD32, CARD32, DeviceIntPtr);

static Bool
apply_bounds (MbSurface *surface, WindowPtr window)
{
    struct MbXdgSurface *xdg = surface->xdg;
    RegionPtr bounds = NULL;

    if (xdg && xdg->geometry_set && surface->pixels)
    {
        Selection *selection;
        int result =
            dixLookupSelection (&selection, xdg->compositor_selection,
                                surface->client->native, DixGetAttrAccess);

        if (result == BadAlloc)
            return FALSE;
        if (result != Success || selection->window == None)
        {
            BoxRec box = { xdg->frame_extents[0], xdg->frame_extents[2],
                           surface->scene_width - xdg->frame_extents[1],
                           surface->scene_height - xdg->frame_extents[3] };
            MbSceneIterator iterator = { .root = surface, .surface = surface };
            MbSurface *node;

            bounds = RegionCreate (
                box.x1 < box.x2 && box.y1 < box.y2 ? &box : NULL, 0);
            if (!bounds)
                return FALSE;
            while ((node = mb_scene_next (&iterator)))
            {
                RegionRec child;
                Bool success;
                int64_t x = iterator.x - surface->scene_x;
                int64_t y = iterator.y - surface->scene_y;

                if (node == surface)
                    continue;
                box =
                    (BoxRec){ max (0, x), max (0, y),
                              min (surface->scene_width, x + node->width),
                              min (surface->scene_height, y + node->height) };
                if (box.x1 >= box.x2 || box.y1 >= box.y2)
                    continue;
                RegionInit (&child, &box, 0);
                success = RegionUnion (bounds, bounds, &child);
                RegionUninit (&child);
                if (!success)
                {
                    RegionDestroy (bounds);
                    return FALSE;
                }
            }
            if (iterator.failed)
            {
                RegionDestroy (bounds);
                return FALSE;
            }
        }
    }
    if ((!bounds && !wBoundingShape (window)) ||
        (bounds && wBoundingShape (window) &&
         RegionEqual (bounds, wBoundingShape (window))))
    {
        if (bounds)
            RegionDestroy (bounds);
        return TRUE;
    }
    if (!MakeWindowOptional (window))
    {
        if (bounds)
            RegionDestroy (bounds);
        return FALSE;
    }
    if (window->optional->boundingShape)
        RegionDestroy (window->optional->boundingShape);
    window->optional->boundingShape = bounds;
    window->drawable.pScreen->SetShape (window, ShapeBounding);
    SendShapeNotify (window, ShapeBounding);
    return TRUE;
}

static void
selection_changed (CallbackListPtr *callbacks, void *data, void *call_data)
{
    SelectionInfoRec *info = call_data;
    MbSurface *surface;

    wl_list_for_each (surface, &compositor.surfaces, link)
    {
        if (surface->xdg &&
            surface->xdg->compositor_selection == info->selection->selection)
        {
            /* Selection destruction callbacks precede clearing the owner. */
            compositor.xdg_shapes_dirty = TRUE;
            return;
        }
    }
}

void
mb_xdg_refresh_shapes (void)
{
    MbSurface *surface;

    if (!compositor.xdg_shapes_dirty)
        return;
    compositor.xdg_shapes_dirty = FALSE;
    wl_list_for_each (surface, &compositor.surfaces, link)
    {
        WindowPtr window;

        if (!surface->xdg)
            continue;
        window = mb_window_get (&surface->window, DixSetAttrAccess);
        if (window)
            mb_surface_update (surface);
    }
}

static void
base_unref (struct XdgBase *base)
{
    if (!--base->refs)
        free (base);
}

static void
clear_configures (struct MbXdgSurface *xdg)
{
    struct Configure *configure, *next;

    wl_list_for_each_safe (configure, next, &xdg->configures, link)
    {
        wl_list_remove (&configure->link);
        free (configure);
    }
    xdg->configure_count = 0;
}

static void
xdg_unref (struct MbXdgSurface *xdg)
{
    if (!--xdg->refs)
    {
        clear_configures (xdg);
        base_unref (xdg->base);
        free (xdg);
    }
}

static void
reparent_children (struct MbXdgSurface *parent)
{
    MbSurface *surface;

    wl_list_for_each (surface, &compositor.surfaces, link)
    {
        WindowPtr window;

        if (!surface->xdg || surface->client->lifetime != parent->owner ||
            surface->xdg->parent != parent->window_id)
            continue;
        CARD32 replacement = parent->parent;

        surface->xdg->parent = replacement;
        window = mb_window_get (&surface->window, DixSetPropAccess);
        if (window)
        {
            if (replacement)
            {
                if (dixChangeWindowProperty (
                        surface->client->native, window, XA_WM_TRANSIENT_FOR,
                        XA_WINDOW, MB_PROPERTY_FORMAT_32, PropModeReplace, 1,
                        &replacement, TRUE) != Success)
                    wl_resource_post_no_memory (surface->resource);
            }
            else
                DeleteProperty (surface->client->native, window,
                                XA_WM_TRANSIENT_FOR);
        }
    }
}

static void
xdg_destroyed (struct wl_resource *resource)
{
    struct MbXdgSurface *xdg = wl_resource_get_user_data (resource);

    xdg->resource = NULL;
    xdg->base->children--;
    if (xdg->surface)
    {
        xdg->surface->xdg = NULL;
        xdg->surface = NULL;
        xdg_unref (xdg);
    }
    xdg_unref (xdg);
}

static void
xdg_destroy (struct wl_client *client, struct wl_resource *resource)
{
    struct MbXdgSurface *xdg = wl_resource_get_user_data (resource);

    if (xdg->toplevel)
    {
        wl_resource_post_error (xdg->base->resource,
                                XDG_WM_BASE_ERROR_INVALID_SURFACE_STATE,
                                "destroy xdg_toplevel before xdg_surface");
        return;
    }
    wl_resource_destroy (resource);
}

static void
reset_mapping (struct MbXdgSurface *xdg)
{
    reparent_children (xdg);
    xdg->initialized = xdg->configured = xdg->map_requested = FALSE;
    xdg->geometry_set = xdg->frame_extents_set = xdg->anchor_set = FALSE;
    xdg->parent = None;
    memset (xdg->frame_extents, 0, sizeof (xdg->frame_extents));
    if (xdg->surface)
    {
        WindowPtr window = mb_window_get (&xdg->surface->window,
                                          DixSetPropAccess | DixSetAttrAccess);
        const Atom properties[] = { compositor.atoms.net_wm_name,
                                    XA_WM_NAME,
                                    XA_WM_CLASS,
                                    XA_WM_TRANSIENT_FOR,
                                    compositor.atoms.net_wm_state,
                                    compositor.atoms.gtk_frame_extents,
                                    compositor.atoms.motif_wm_hints,
                                    XA_WM_NORMAL_HINTS };
        unsigned int i;

        if (window)
        {
            if (!apply_bounds (xdg->surface, window))
                wl_resource_post_no_memory (xdg->surface->resource);
            for (i = 0; i < ARRAY_SIZE (properties); i++)
                DeleteProperty (xdg->surface->client->native, window,
                                properties[i]);
        }
    }
    xdg->min_width = xdg->min_height = xdg->max_width = xdg->max_height = 0;
    xdg->hints_changed = TRUE;
    clear_configures (xdg);
}

static void
toplevel_destroyed (struct wl_resource *resource)
{
    struct MbXdgSurface *xdg = wl_resource_get_user_data (resource);
    WindowPtr window =
        xdg->surface ? mb_window_get (&xdg->surface->window, DixSetAttrAccess)
                     : NULL;

    xdg->toplevel = NULL;
    if (window)
        UnmapWindow (window, FALSE);
    reset_mapping (xdg);
    xdg_unref (xdg);
}

static void
toplevel_destroy (struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_destroy (resource);
}

static struct MbXdgSurface *
active_toplevel (struct wl_resource *resource)
{
    struct MbXdgSurface *xdg = wl_resource_get_user_data (resource);

    if (!xdg->surface || !xdg->surface->client->native ||
        xdg->surface->client->failed)
        return NULL;
    return xdg;
}

static void
set_parent (struct wl_client *client, struct wl_resource *resource,
            struct wl_resource *parent_resource)
{
    struct MbXdgSurface *xdg = active_toplevel (resource);
    struct MbXdgSurface *parent =
        parent_resource ? active_toplevel (parent_resource) : NULL;
    WindowPtr window;
    CARD32 id;
    XID ancestor;
    int remaining = wl_list_length (&compositor.surfaces);

    if (!xdg)
        return;
    if (parent == xdg)
    {
        wl_resource_post_error (resource, XDG_TOPLEVEL_ERROR_INVALID_PARENT,
                                "self parent");
        return;
    }
    if (parent && !parent->map_requested)
        parent = NULL;
    ancestor = parent ? parent->window_id : None;
    while (ancestor && remaining--)
    {
        MbSurface *surface;
        XID next = None;

        if (ancestor == xdg->window_id)
        {
            wl_resource_post_error (resource,
                                    XDG_TOPLEVEL_ERROR_INVALID_PARENT,
                                    "cyclic toplevel parent");
            return;
        }
        wl_list_for_each (surface, &compositor.surfaces, link)
        {
            if (surface->window.id == ancestor && surface->xdg)
            {
                next = surface->xdg->parent;
                break;
            }
        }
        ancestor = next;
    }
    window = mb_window_get (&xdg->surface->window, DixSetPropAccess);
    if (!window)
        return;
    id = parent ? parent->window_id : None;
    if (id)
    {
        if (dixChangeWindowProperty (xdg->surface->client->native, window,
                                     XA_WM_TRANSIENT_FOR, XA_WINDOW,
                                     MB_PROPERTY_FORMAT_32, PropModeReplace, 1,
                                     &id, TRUE) != Success)
        {
            wl_resource_post_no_memory (resource);
            return;
        }
    }
    else
        DeleteProperty (xdg->surface->client->native, window,
                        XA_WM_TRANSIENT_FOR);
    xdg->parent = id;
}

static void
set_title (struct wl_client *client, struct wl_resource *resource,
           const char *title)
{
    struct MbXdgSurface *xdg = active_toplevel (resource);
    WindowPtr window;

    if (!xdg ||
        !(window = mb_window_get (&xdg->surface->window, DixSetPropAccess)))
        return;
    if (dixChangeWindowProperty (
            xdg->surface->client->native, window, compositor.atoms.net_wm_name,
            compositor.atoms.utf8_string, MB_PROPERTY_FORMAT_8,
            PropModeReplace, strlen (title), title, TRUE) != Success ||
        dixChangeWindowProperty (xdg->surface->client->native, window,
                                 XA_WM_NAME, XA_STRING, MB_PROPERTY_FORMAT_8,
                                 PropModeReplace, strlen (title), title,
                                 TRUE) != Success)
        wl_resource_post_no_memory (resource);
}

static void
set_app_id (struct wl_client *client, struct wl_resource *resource,
            const char *app_id)
{
    struct MbXdgSurface *xdg = active_toplevel (resource);
    WindowPtr window;
    size_t length = strlen (app_id) + 1;
    char *class;

    if (!xdg ||
        !(window = mb_window_get (&xdg->surface->window, DixSetPropAccess)))
        return;
    class = malloc (length * 2);
    if (!class)
    {
        wl_resource_post_no_memory (resource);
        return;
    }
    memcpy (class, app_id, length);
    memcpy (class + length, app_id, length);
    if (dixChangeWindowProperty (xdg->surface->client->native, window,
                                 XA_WM_CLASS, XA_STRING, MB_PROPERTY_FORMAT_8,
                                 PropModeReplace, length * 2, class,
                                 TRUE) != Success)
        wl_resource_post_no_memory (resource);
    free (class);
}

static void
show_menu (struct wl_client *client, struct wl_resource *resource,
           struct wl_resource *seat, uint32_t serial, int32_t x, int32_t y)
{
}

static void
interactive (struct MbXdgSurface *xdg, struct wl_resource *seat,
             uint32_t serial, enum WmMoveResize direction)
{
    MbInteractiveGrab grab;

    if (!xdg || !xdg->map_requested || !xdg->surface->pixels ||
        !mb_input_take_grab (xdg->surface, seat, serial, &grab))
        return;
    wm_message (xdg, compositor.atoms.net_wm_moveresize, grab.x, grab.y,
                direction, grab.button, WM_SOURCE_APPLICATION, grab.pointer);
}

static void
move (struct wl_client *client, struct wl_resource *resource,
      struct wl_resource *seat, uint32_t serial)
{
    interactive (active_toplevel (resource), seat, serial, WM_MOVE);
}

static void
resize (struct wl_client *client, struct wl_resource *resource,
        struct wl_resource *seat, uint32_t serial, uint32_t edges)
{
    enum WmMoveResize direction;

    switch (edges)
    {
    case XDG_TOPLEVEL_RESIZE_EDGE_NONE:
        return;
    case XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT:
        direction = WM_RESIZE_TOP_LEFT;
        break;
    case XDG_TOPLEVEL_RESIZE_EDGE_TOP:
        direction = WM_RESIZE_TOP;
        break;
    case XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT:
        direction = WM_RESIZE_TOP_RIGHT;
        break;
    case XDG_TOPLEVEL_RESIZE_EDGE_RIGHT:
        direction = WM_RESIZE_RIGHT;
        break;
    case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT:
        direction = WM_RESIZE_BOTTOM_RIGHT;
        break;
    case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM:
        direction = WM_RESIZE_BOTTOM;
        break;
    case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT:
        direction = WM_RESIZE_BOTTOM_LEFT;
        break;
    case XDG_TOPLEVEL_RESIZE_EDGE_LEFT:
        direction = WM_RESIZE_LEFT;
        break;
    default:
        wl_resource_post_error (resource,
                                XDG_TOPLEVEL_ERROR_INVALID_RESIZE_EDGE,
                                "invalid resize edge");
        return;
    }
    interactive (active_toplevel (resource), seat, serial, direction);
}

static void
set_max_size (struct wl_client *client, struct wl_resource *resource,
              int32_t width, int32_t height)
{
    struct MbXdgSurface *xdg = active_toplevel (resource);

    if (!xdg)
        return;
    if (width < 0 || height < 0)
    {
        wl_resource_post_error (resource, XDG_TOPLEVEL_ERROR_INVALID_SIZE,
                                "negative maximum size");
        return;
    }
    if (xdg->max_width != width || xdg->max_height != height)
    {
        xdg->max_width = width;
        xdg->max_height = height;
        xdg->hints_changed = TRUE;
    }
}

static void
set_min_size (struct wl_client *client, struct wl_resource *resource,
              int32_t width, int32_t height)
{
    struct MbXdgSurface *xdg = active_toplevel (resource);

    if (!xdg)
        return;
    if (width < 0 || height < 0)
    {
        wl_resource_post_error (resource, XDG_TOPLEVEL_ERROR_INVALID_SIZE,
                                "negative minimum size");
        return;
    }
    if (xdg->min_width != width || xdg->min_height != height)
    {
        xdg->min_width = width;
        xdg->min_height = height;
        xdg->hints_changed = TRUE;
    }
}

static void
wm_message (struct MbXdgSurface *xdg, Atom type, CARD32 first, CARD32 second,
            CARD32 third, CARD32 fourth, CARD32 fifth, DeviceIntPtr pointer)
{
    MbSurface *surface;
    WindowPtr root;
    xEvent event = { 0 };

    if (!xdg)
        return;
    surface = xdg->surface;
    if (dixLookupWindow (&root, surface->window.screen->root->drawable.id,
                         surface->client->native, DixSendAccess) != Success)
        return;
    event.u.u.type = ClientMessage | SEND_EVENT_MASK;
    event.u.u.detail = MB_PROPERTY_FORMAT_32;
    event.u.clientMessage.window = surface->window.id;
    event.u.clientMessage.u.l.type = type;
    event.u.clientMessage.u.l.longs0 = first;
    event.u.clientMessage.u.l.longs1 = second;
    event.u.clientMessage.u.l.longs2 = third;
    event.u.clientMessage.u.l.longs3 = fourth;
    event.u.clientMessage.u.l.longs4 = fifth;
    if (XaceHookSendAccess (surface->client->native, NULL, root, &event, 1) ==
        Success)
        DeliverEventsToWindow (
            pointer ? pointer : PickPointer (surface->client->native), root,
            &event, 1, SubstructureRedirectMask | SubstructureNotifyMask,
            NullGrab);
}

static void
wm_state (struct MbXdgSurface *xdg, CARD32 action, Atom first, Atom second)
{
    WindowPtr window;
    PropertyPtr property;
    CARD32 *atoms;
    size_t count = 0, i;
    Bool have_first = FALSE, have_second = FALSE;

    if (!xdg)
        return;
    if (xdg->map_requested)
    {
        wm_message (xdg, compositor.atoms.net_wm_state, action, first, second,
                    WM_SOURCE_APPLICATION, 0, NULL);
        return;
    }
    window = mb_window_get (&xdg->surface->window,
                            DixSetPropAccess | DixGetPropAccess);
    if (!window)
        return;
    if (dixLookupProperty (&property, window, compositor.atoms.net_wm_state,
                           xdg->surface->client->native,
                           DixReadAccess) == Success &&
        property->type == XA_ATOM && property->format == MB_PROPERTY_FORMAT_32)
        count = property->size;
    else
        property = NULL;
    if (count > WM_STATE_ATOM_LIMIT)
        goto fail;
    atoms = calloc (count + 2, sizeof (*atoms));
    if (!atoms)
        goto fail;
    count = 0;
    if (property)
    {
        const CARD32 *previous = property->data;

        for (i = 0; i < property->size; i++)
        {
            have_first |= previous[i] == first;
            have_second |= previous[i] == second;
            if (action == WM_STATE_REMOVE &&
                (previous[i] == first || previous[i] == second))
                continue;
            atoms[count++] = previous[i];
        }
    }
    if (action == WM_STATE_ADD)
    {
        if (first && !have_first)
            atoms[count++] = first;
        if (second && !have_second)
            atoms[count++] = second;
    }
    int status = dixChangeWindowProperty (
        xdg->surface->client->native, window, compositor.atoms.net_wm_state,
        XA_ATOM, MB_PROPERTY_FORMAT_32, PropModeReplace, count, atoms, TRUE);
    free (atoms);
    if (status == Success)
        return;
fail:
    wl_resource_post_no_memory (xdg->toplevel);
}

static void
set_maximized (struct wl_client *client, struct wl_resource *resource)
{
    wm_state (active_toplevel (resource), WM_STATE_ADD,
              compositor.atoms.net_wm_maximized_horz,
              compositor.atoms.net_wm_maximized_vert);
}

static void
unset_maximized (struct wl_client *client, struct wl_resource *resource)
{
    wm_state (active_toplevel (resource), WM_STATE_REMOVE,
              compositor.atoms.net_wm_maximized_horz,
              compositor.atoms.net_wm_maximized_vert);
}

static void
set_fullscreen (struct wl_client *client, struct wl_resource *resource,
                struct wl_resource *output)
{
    wm_state (active_toplevel (resource), WM_STATE_ADD,
              compositor.atoms.net_wm_fullscreen, None);
}

static void
unset_fullscreen (struct wl_client *client, struct wl_resource *resource)
{
    wm_state (active_toplevel (resource), WM_STATE_REMOVE,
              compositor.atoms.net_wm_fullscreen, None);
}

static void
set_minimized (struct wl_client *client, struct wl_resource *resource)
{
    wm_message (active_toplevel (resource), compositor.atoms.wm_change_state,
                WM_ICONIC_STATE, 0, 0, 0, 0, NULL);
}

static const struct xdg_toplevel_interface toplevel_implementation = {
    .destroy = toplevel_destroy,
    .set_parent = set_parent,
    .set_title = set_title,
    .set_app_id = set_app_id,
    .show_window_menu = show_menu,
    .move = move,
    .resize = resize,
    .set_max_size = set_max_size,
    .set_min_size = set_min_size,
    .set_maximized = set_maximized,
    .unset_maximized = unset_maximized,
    .set_fullscreen = set_fullscreen,
    .unset_fullscreen = unset_fullscreen,
    .set_minimized = set_minimized,
};

static void
get_toplevel (struct wl_client *client, struct wl_resource *resource,
              uint32_t id)
{
    struct MbXdgSurface *xdg = wl_resource_get_user_data (resource);

    if (!xdg->surface)
    {
        wl_resource_post_error (resource, XDG_SURFACE_ERROR_NOT_CONSTRUCTED,
                                "wl_surface was destroyed");
        return;
    }
    if (xdg->toplevel)
    {
        wl_resource_post_error (resource,
                                XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED,
                                "xdg role already exists");
        return;
    }
    xdg->toplevel =
        wl_resource_create (client, &xdg_toplevel_interface, XDG_VERSION, id);
    if (!xdg->toplevel)
    {
        wl_resource_post_no_memory (resource);
        return;
    }
    xdg->refs++;
    xdg->hints_changed = TRUE;
    wl_resource_set_implementation (xdg->toplevel, &toplevel_implementation,
                                    xdg, toplevel_destroyed);
}

static void
get_popup (struct wl_client *client, struct wl_resource *resource, uint32_t id,
           struct wl_resource *parent, struct wl_resource *positioner)
{
    struct MbXdgSurface *xdg = wl_resource_get_user_data (resource);

    wl_resource_post_error (xdg->base->resource,
                            XDG_WM_BASE_ERROR_INVALID_SURFACE_STATE,
                            "popup roles are unavailable");
}

static void
set_geometry (struct wl_client *client, struct wl_resource *resource,
              int32_t x, int32_t y, int32_t width, int32_t height)
{
    struct MbXdgSurface *xdg = wl_resource_get_user_data (resource);

    if (!xdg->toplevel)
    {
        wl_resource_post_error (resource, XDG_SURFACE_ERROR_NOT_CONSTRUCTED,
                                "xdg surface requires a role");
        return;
    }
    if (width <= 0 || height <= 0)
    {
        wl_resource_post_error (resource, XDG_SURFACE_ERROR_INVALID_SIZE,
                                "invalid window geometry");
        return;
    }
    xdg->geometry_set = TRUE;
    xdg->geometry_x = x;
    xdg->geometry_y = y;
    xdg->geometry_width = width;
    xdg->geometry_height = height;
}

static void
ack_configure (struct wl_client *client, struct wl_resource *resource,
               uint32_t serial)
{
    struct MbXdgSurface *xdg = wl_resource_get_user_data (resource);
    struct Configure *configure, *next, *match = NULL;

    if (!xdg->toplevel)
    {
        wl_resource_post_error (resource, XDG_SURFACE_ERROR_NOT_CONSTRUCTED,
                                "xdg surface requires a role");
        return;
    }
    wl_list_for_each (configure, &xdg->configures, link)
    {
        if (configure->serial == serial)
        {
            match = configure;
            break;
        }
    }
    if (!match)
    {
        wl_resource_post_error (resource, XDG_SURFACE_ERROR_INVALID_SERIAL,
                                "unknown configure serial");
        return;
    }
    wl_list_for_each_safe (configure, next, &xdg->configures, link)
    {
        Bool done = configure == match;

        wl_list_remove (&configure->link);
        free (configure);
        xdg->configure_count--;
        if (done)
            break;
    }
    xdg->configured = TRUE;
}

static const struct xdg_surface_interface xdg_implementation = {
    .destroy = xdg_destroy,
    .get_toplevel = get_toplevel,
    .get_popup = get_popup,
    .set_window_geometry = set_geometry,
    .ack_configure = ack_configure,
};

static void
get_xdg_surface (struct wl_client *client, struct wl_resource *resource,
                 uint32_t id, struct wl_resource *surface_resource)
{
    struct XdgBase *base = wl_resource_get_user_data (resource);
    MbSurface *surface = wl_resource_get_user_data (surface_resource);
    struct MbXdgSurface *xdg;
    char selection_name[sizeof (COMPOSITOR_SELECTION_FORMAT) +
                        sizeof (int) * CHAR_BIT];

    if (surface->xdg || surface->buffer || surface->pending_buffer ||
        surface->role == MB_ROLE_SUBSURFACE)
    {
        wl_resource_post_error (resource,
                                XDG_WM_BASE_ERROR_INVALID_SURFACE_STATE,
                                "surface already has a role or buffer");
        return;
    }
    xdg = calloc (1, sizeof (*xdg));
    if (!xdg)
    {
        wl_resource_post_no_memory (resource);
        return;
    }
    snprintf (selection_name, sizeof (selection_name),
              COMPOSITOR_SELECTION_FORMAT, surface->window.screen->myNum);
    xdg->compositor_selection =
        MakeAtom (selection_name, strlen (selection_name), TRUE);
    if (!xdg->compositor_selection)
    {
        free (xdg);
        wl_resource_post_no_memory (resource);
        return;
    }
    xdg->resource =
        wl_resource_create (client, &xdg_surface_interface, XDG_VERSION, id);
    if (!xdg->resource)
    {
        free (xdg);
        wl_resource_post_no_memory (resource);
        return;
    }
    xdg->base = base;
    base->refs++;
    base->children++;
    xdg->surface = surface;
    xdg->window_id = surface->window.id;
    xdg->owner = surface->client->lifetime;
    xdg->refs = 2;
    wl_list_init (&xdg->configures);
    surface->xdg = xdg;
    surface->role = MB_ROLE_XDG;
    wl_resource_set_implementation (xdg->resource, &xdg_implementation, xdg,
                                    xdg_destroyed);
}

static void
base_destroyed (struct wl_resource *resource)
{
    struct XdgBase *base = wl_resource_get_user_data (resource);

    base->resource = NULL;
    base_unref (base);
}

static void
base_destroy (struct wl_client *client, struct wl_resource *resource)
{
    struct XdgBase *base = wl_resource_get_user_data (resource);

    if (base->children)
    {
        wl_resource_post_error (resource, XDG_WM_BASE_ERROR_DEFUNCT_SURFACES,
                                "xdg surfaces remain alive");
        return;
    }
    wl_resource_destroy (resource);
}

static void
create_positioner (struct wl_client *client, struct wl_resource *resource,
                   uint32_t id)
{
    wl_resource_post_error (resource, XDG_WM_BASE_ERROR_INVALID_POSITIONER,
                            "popup roles are unavailable");
}

static void
pong (struct wl_client *client, struct wl_resource *resource, uint32_t serial)
{
}

static const struct xdg_wm_base_interface implementation = {
    .destroy = base_destroy,
    .create_positioner = create_positioner,
    .get_xdg_surface = get_xdg_surface,
    .pong = pong,
};

static void
bind_xdg (struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    struct XdgBase *base = calloc (1, sizeof (*base));

    if (!base)
    {
        wl_client_post_no_memory (client);
        return;
    }
    base->resource =
        wl_resource_create (client, &xdg_wm_base_interface, XDG_VERSION, id);
    if (!base->resource)
    {
        free (base);
        wl_client_post_no_memory (client);
        return;
    }
    base->refs = 1;
    wl_resource_set_implementation (base->resource, &implementation, base,
                                    base_destroyed);
}

static Bool
send_configure (struct MbXdgSurface *xdg, int32_t width, int32_t height)
{
    struct Configure *configure;
    WindowPtr window = mb_window_get (&xdg->surface->window, DixGetPropAccess);
    PropertyPtr property;
    CARD32 states[MAX_TOPLEVEL_STATES];
    size_t count = 0;
    Bool horizontal = FALSE, vertical = FALSE, fullscreen = FALSE,
         focused = FALSE;
    struct wl_array array = { 0 };

    if (xdg->configure_count == CONFIGURE_LIMIT)
        return FALSE;
    configure = calloc (1, sizeof (*configure));
    if (!configure)
        return FALSE;
    if (window &&
        dixLookupProperty (&property, window, compositor.atoms.net_wm_state,
                           xdg->surface->client->native,
                           DixReadAccess) == Success &&
        property->type == XA_ATOM && property->format == MB_PROPERTY_FORMAT_32)
    {
        unsigned int i;
        const CARD32 *atoms = property->data;

        for (i = 0; i < property->size; i++)
        {
            horizontal |= atoms[i] == compositor.atoms.net_wm_maximized_horz;
            vertical |= atoms[i] == compositor.atoms.net_wm_maximized_vert;
            fullscreen |= atoms[i] == compositor.atoms.net_wm_fullscreen;
            focused |= atoms[i] == compositor.atoms.net_wm_focused;
        }
    }
    if (horizontal && vertical)
        states[count++] = XDG_TOPLEVEL_STATE_MAXIMIZED;
    if (fullscreen)
        states[count++] = XDG_TOPLEVEL_STATE_FULLSCREEN;
    if (focused)
        states[count++] = XDG_TOPLEVEL_STATE_ACTIVATED;
    array.size = count * sizeof (*states);
    array.data = states;
    configure->serial = wl_display_next_serial (compositor.wl_display);
    wl_list_insert (xdg->configures.prev, &configure->link);
    xdg->configure_count++;
    xdg->last_width = width;
    xdg->last_height = height;
    xdg_toplevel_send_configure (xdg->toplevel, width, height, &array);
    xdg_surface_send_configure (xdg->resource, configure->serial);
    return TRUE;
}

Bool
mb_xdg_commit (MbSurface *surface)
{
    struct MbXdgSurface *xdg = surface->xdg;

    if (!xdg)
        return TRUE;
    if (!xdg->toplevel)
    {
        wl_resource_post_error (xdg->resource,
                                XDG_SURFACE_ERROR_NOT_CONSTRUCTED,
                                "xdg surface requires a role");
        return FALSE;
    }
    if ((surface->pending_buffer || surface->buffer) && !xdg->configured)
    {
        wl_resource_post_error (
            xdg->resource, XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER,
            "buffer committed before configure acknowledgement");
        return FALSE;
    }
    if ((xdg->max_width && xdg->min_width > xdg->max_width) ||
        (xdg->max_height && xdg->min_height > xdg->max_height))
    {
        wl_resource_post_error (xdg->toplevel, XDG_TOPLEVEL_ERROR_INVALID_SIZE,
                                "minimum exceeds maximum size");
        return FALSE;
    }
    if (!xdg->initialized)
    {
        if (!send_configure (xdg, 0, 0))
        {
            wl_resource_post_no_memory (xdg->resource);
            return FALSE;
        }
        xdg->initialized = TRUE;
    }
    return TRUE;
}

Bool
mb_xdg_constrain_scene (MbSurface *surface, pixman_box32_t *bounds,
                        int32_t *anchor_x, int32_t *anchor_y)
{
    struct MbXdgSurface *xdg = surface->xdg;
    WindowPtr window;
    PropertyPtr property;
    Selection *selection;
    Bool horizontal = FALSE, vertical = FALSE, fullscreen = FALSE;
    Bool constrained;
    pixman_box32_t geometry;
    int result;

    *anchor_x = *anchor_y = 0;
    if (!xdg || !xdg->geometry_set || !surface->pixels)
        return TRUE;
    window = mb_window_get (&surface->window, DixGetPropAccess);
    if (!window)
        return TRUE;
    result =
        dixLookupProperty (&property, window, compositor.atoms.net_wm_state,
                           surface->client->native, DixReadAccess);
    if (result == BadAlloc)
        return FALSE;
    if (result == Success && property->type == XA_ATOM &&
        property->format == MB_PROPERTY_FORMAT_32)
    {
        const CARD32 *atoms = property->data;
        unsigned int i;

        for (i = 0; i < property->size; i++)
        {
            horizontal |= atoms[i] == compositor.atoms.net_wm_maximized_horz;
            vertical |= atoms[i] == compositor.atoms.net_wm_maximized_vert;
            fullscreen |= atoms[i] == compositor.atoms.net_wm_fullscreen;
        }
    }
    constrained = fullscreen || (horizontal && vertical);
    geometry = (pixman_box32_t){
        max (bounds->x1, min (bounds->x2, xdg->geometry_x)),
        max (bounds->y1, min (bounds->y2, xdg->geometry_y)),
        min ((int64_t)bounds->x2,
             max ((int64_t)bounds->x1,
                  (int64_t)xdg->geometry_x + xdg->geometry_width)),
        min ((int64_t)bounds->y2,
             max ((int64_t)bounds->y1,
                  (int64_t)xdg->geometry_y + xdg->geometry_height)),
    };
    if (xdg->anchor_set)
    {
        *anchor_x = xdg->anchor_x - geometry.x1;
        *anchor_y = xdg->anchor_y - geometry.y1;
    }
    xdg->anchor_x = geometry.x1;
    xdg->anchor_y = geometry.y1;
    xdg->anchor_set = TRUE;
    if (geometry.x1 >= geometry.x2 || geometry.y1 >= geometry.y2)
        return TRUE;
    if (!constrained)
    {
        MbSceneIterator iterator = { .root = surface, .surface = surface };
        MbSurface *node;

        result =
            dixLookupSelection (&selection, xdg->compositor_selection,
                                surface->client->native, DixGetAttrAccess);
        if (result == BadAlloc)
            return FALSE;
        if (result == Success && selection->window != None)
            return TRUE;
        while ((node = mb_scene_next (&iterator)))
        {
            if (node == surface)
                continue;
            geometry.x1 = min ((int64_t)geometry.x1, iterator.x);
            geometry.y1 = min ((int64_t)geometry.y1, iterator.y);
            geometry.x2 = max ((int64_t)geometry.x2, iterator.x + node->width);
            geometry.y2 =
                max ((int64_t)geometry.y2, iterator.y + node->height);
        }
        if (iterator.failed)
            return FALSE;
    }
    *bounds = geometry;
    return TRUE;
}

Bool
mb_xdg_map (MbSurface *surface)
{
    struct MbXdgSurface *xdg = surface->xdg;
    WindowPtr window;

    if (!xdg || !xdg->toplevel)
        return TRUE;
    window =
        mb_window_get (&surface->window, DixManageAccess | DixSetAttrAccess);
    if (!window)
        return FALSE;
    if (xdg->geometry_set && surface->pixels)
    {
        int64_t x2 = (int64_t)xdg->geometry_x + xdg->geometry_width;
        int64_t y2 = (int64_t)xdg->geometry_y + xdg->geometry_height;
        int32_t right = surface->scene_x + surface->scene_width;
        int32_t bottom = surface->scene_y + surface->scene_height;
        CARD32 extents[] = {
            min (right, max (surface->scene_x, xdg->geometry_x)) -
                surface->scene_x,
            right - min (right, max (surface->scene_x, x2)),
            min (bottom, max (surface->scene_y, xdg->geometry_y)) -
                surface->scene_y,
            bottom - min (bottom, max (surface->scene_y, y2)),
        };

        if (!xdg->frame_extents_set ||
            memcmp (xdg->frame_extents, extents, sizeof (extents)))
        {
            xdg->hints_changed |=
                xdg->frame_extents[0] + xdg->frame_extents[1] !=
                    extents[0] + extents[1] ||
                xdg->frame_extents[2] + xdg->frame_extents[3] !=
                    extents[2] + extents[3];
            if (dixChangeWindowProperty (surface->client->native, window,
                                         compositor.atoms.gtk_frame_extents,
                                         XA_CARDINAL, MB_PROPERTY_FORMAT_32,
                                         PropModeReplace, ARRAY_SIZE (extents),
                                         extents, TRUE) != Success)
                return FALSE;
            memcpy (xdg->frame_extents, extents, sizeof (extents));
            xdg->frame_extents_set = TRUE;
        }
    }
    if (!apply_bounds (surface, window))
        return FALSE;
    if (xdg->hints_changed)
    {
        CARD32 hints[WM_SIZE_HINT_WORDS] = { 0 };
        CARD32 horizontal = xdg->frame_extents[0] + xdg->frame_extents[1];
        CARD32 vertical = xdg->frame_extents[2] + xdg->frame_extents[3];

        if (horizontal || vertical)
        {
            hints[WM_HINT_FLAGS] |= WM_HINT_BASE_SIZE | WM_HINT_RESIZE_INC;
            hints[WM_HINT_BASE_WIDTH] = horizontal;
            hints[WM_HINT_BASE_HEIGHT] = vertical;
            hints[WM_HINT_WIDTH_INC] = WM_RESIZE_INCREMENT;
            hints[WM_HINT_HEIGHT_INC] = WM_RESIZE_INCREMENT;
        }
        if (xdg->min_width || xdg->min_height || horizontal || vertical)
            hints[WM_HINT_FLAGS] |= WM_HINT_MIN_SIZE;
        if (xdg->max_width || xdg->max_height)
            hints[WM_HINT_FLAGS] |= WM_HINT_MAX_SIZE;
        hints[WM_HINT_MIN_WIDTH] =
            min (MB_MAX_DIMENSION, max (1, xdg->min_width) +
                                       (int64_t)xdg->frame_extents[0] +
                                       xdg->frame_extents[1]);
        hints[WM_HINT_MIN_HEIGHT] =
            min (MB_MAX_DIMENSION, max (1, xdg->min_height) +
                                       (int64_t)xdg->frame_extents[2] +
                                       xdg->frame_extents[3]);
        hints[WM_HINT_MAX_WIDTH] =
            xdg->max_width ? min (MB_MAX_DIMENSION, (int64_t)xdg->max_width +
                                                        xdg->frame_extents[0] +
                                                        xdg->frame_extents[1])
                           : MB_MAX_DIMENSION;
        hints[WM_HINT_MAX_HEIGHT] =
            xdg->max_height
                ? min (MB_MAX_DIMENSION, (int64_t)xdg->max_height +
                                             xdg->frame_extents[2] +
                                             xdg->frame_extents[3])
                : MB_MAX_DIMENSION;
        if (dixChangeWindowProperty (
                surface->client->native, window, XA_WM_NORMAL_HINTS,
                XA_WM_SIZE_HINTS, MB_PROPERTY_FORMAT_32, PropModeReplace,
                ARRAY_SIZE (hints), hints, TRUE) != Success)
            return FALSE;
        xdg->hints_changed = FALSE;
    }
    if (surface->pixels && !xdg->map_requested)
    {
        CARD32 hints[MOTIF_HINT_WORDS] = {
            [MOTIF_HINT_FLAGS] = MOTIF_HINT_DECORATIONS,
        };

        /* Clients self-decorate until a server-side mode is negotiated. */
        if (dixChangeWindowProperty (
                surface->client->native, window,
                compositor.atoms.motif_wm_hints,
                compositor.atoms.motif_wm_hints, MB_PROPERTY_FORMAT_32,
                PropModeReplace, ARRAY_SIZE (hints), hints, TRUE) != Success)
            return FALSE;
        if (MapWindow (window, surface->client->native) != Success)
            return FALSE;
        xdg->map_requested = TRUE;
    }
    else if (!surface->pixels && xdg->map_requested)
        reset_mapping (xdg);
    return TRUE;
}

Bool
mb_xdg_surface_destroy_request (MbSurface *surface)
{
    if (surface->xdg && surface->xdg->toplevel)
    {
        wl_resource_post_error (surface->xdg->base->resource,
                                XDG_WM_BASE_ERROR_INVALID_SURFACE_STATE,
                                "destroy xdg_toplevel before wl_surface");
        return FALSE;
    }
    return TRUE;
}

void
mb_xdg_surface_destroyed (MbSurface *surface)
{
    struct MbXdgSurface *xdg = surface->xdg;

    if (xdg)
    {
        reparent_children (xdg);
        xdg->surface = NULL;
        surface->xdg = NULL;
        xdg_unref (xdg);
    }
}

void
mb_xdg_event (MbSurface *surface, const xEvent *event)
{
    struct MbXdgSurface *xdg = surface->xdg;
    WindowPtr window;
    unsigned int type = event->u.u.type & ~SEND_EVENT_MASK;

    if (!xdg || !xdg->toplevel)
        return;
    if (type == ClientMessage && event->u.u.detail == MB_PROPERTY_FORMAT_32 &&
        event->u.clientMessage.u.l.type == compositor.atoms.wm_protocols &&
        event->u.clientMessage.u.l.longs0 == compositor.atoms.wm_delete_window)
    {
        xdg_toplevel_send_close (xdg->toplevel);
        return;
    }
    window = mb_window_get (&surface->window, DixGetAttrAccess);
    if (!window || !xdg->initialized)
        return;
    if (type == PropertyNotify &&
        event->u.property.atom == compositor.atoms.net_wm_state &&
        !mb_surface_update (surface))
        return;
    int32_t width = max (0, (int64_t)window->drawable.width -
                                xdg->frame_extents[0] - xdg->frame_extents[1]);
    int32_t height =
        max (0, (int64_t)window->drawable.height - xdg->frame_extents[2] -
                    xdg->frame_extents[3]);

    if ((type == ConfigureNotify &&
         (xdg->last_width != width || xdg->last_height != height)) ||
        (type == PropertyNotify && xdg->map_requested &&
         event->u.property.atom == compositor.atoms.net_wm_state))
    {
        if (!send_configure (xdg, width, height))
            wl_resource_post_no_memory (xdg->resource);
    }
}

Bool
mb_xdg_initialize (void)
{
    compositor.xdg_global =
        wl_global_create (compositor.wl_display, &xdg_wm_base_interface,
                          XDG_VERSION, NULL, bind_xdg);
    if (!compositor.xdg_global)
        return FALSE;
    if (!AddCallback (&SelectionCallback, selection_changed, NULL))
    {
        wl_global_destroy (compositor.xdg_global);
        compositor.xdg_global = NULL;
        return FALSE;
    }
    return TRUE;
}

void
mb_xdg_shutdown (void)
{
    if (compositor.xdg_global)
    {
        DeleteCallback (&SelectionCallback, selection_changed, NULL);
        wl_global_destroy (compositor.xdg_global);
        compositor.xdg_global = NULL;
    }
    compositor.xdg_shapes_dirty = FALSE;
}
