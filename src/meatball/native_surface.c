/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "server.h"

#include <X11/extensions/shapeconst.h>
#include <limits.h>
#include <stdlib.h>
#include <wayland-server-protocol.h>
#include <window.h>

struct Frame
{
    struct wl_list link;
    struct wl_resource *resource;
};

static void
frame_destroyed (struct wl_resource *resource)
{
    struct Frame *frame = wl_resource_get_user_data (resource);

    wl_list_remove (&frame->link);
    free (frame);
}

static void
surface_destroyed (struct wl_resource *resource)
{
    MbSurface *surface = wl_resource_get_user_data (resource);
    struct Frame *frame, *next;

    mb_window_destroy (&surface->window);
    wl_list_for_each_safe (frame, next, &surface->pending_frames, link)
    {
        wl_resource_destroy (frame->resource);
    }
    wl_list_for_each_safe (frame, next, &surface->frames, link)
    {
        wl_resource_destroy (frame->resource);
    }
    pixman_region32_fini (&surface->pending_input);
    pixman_region32_fini (&surface->input);
    wl_list_remove (&surface->link);
    free (surface);
}

static void
surface_destroy (struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_destroy (resource);
}

static void
surface_attach (struct wl_client *client, struct wl_resource *resource,
                struct wl_resource *buffer, int32_t x, int32_t y)
{
    MbSurface *surface = wl_resource_get_user_data (resource);

    if (buffer)
    {
        wl_client_post_implementation_error (
            client, "unsupported buffer implementation");
        return;
    }
    surface->detach_pending = TRUE;
}

static void
surface_damage (struct wl_client *client, struct wl_resource *resource,
                int32_t x, int32_t y, int32_t width, int32_t height)
{
    /* Rendering consumes the complete buffer. */
}

static void
surface_frame (struct wl_client *client, struct wl_resource *resource,
               uint32_t id)
{
    MbSurface *surface = wl_resource_get_user_data (resource);
    struct Frame *frame = calloc (1, sizeof (*frame));

    if (!frame)
    {
        wl_resource_post_no_memory (resource);
        return;
    }
    frame->resource =
        wl_resource_create (client, &wl_callback_interface, 1, id);
    if (!frame->resource)
    {
        free (frame);
        wl_resource_post_no_memory (resource);
        return;
    }
    wl_list_insert (surface->pending_frames.prev, &frame->link);
    wl_resource_set_implementation (frame->resource, NULL, frame,
                                    frame_destroyed);
}

static void
surface_opaque (struct wl_client *client, struct wl_resource *resource,
                struct wl_resource *region)
{
    /* Every pixel retains its buffer's alpha value. */
}

static void
surface_input (struct wl_client *client, struct wl_resource *resource,
               struct wl_resource *region)
{
    MbSurface *surface = wl_resource_get_user_data (resource);

    if (region && !pixman_region32_copy (&surface->pending_input,
                                         wl_resource_get_user_data (region)))
    {
        wl_resource_post_no_memory (resource);
        return;
    }
    surface->pending_input_default = !region;
    surface->input_changed = TRUE;
}

static Bool
apply_input (MbSurface *surface, WindowPtr native)
{
    RegionPtr input = NULL;

    if (!surface->input_default)
    {
        int count, i, valid = 0;
        pixman_box32_t *boxes =
            pixman_region32_rectangles (&surface->input, &count);
        BoxPtr clipped = count ? calloc (count, sizeof (*clipped)) : NULL;

        if (count && !clipped)
            return FALSE;
        for (i = 0; i < count; i++)
        {
            int x1 = max (boxes[i].x1, 0);
            int y1 = max (boxes[i].y1, 0);
            int x2 = min (boxes[i].x2,
                          min (native->drawable.width, MB_MAX_DIMENSION));
            int y2 = min (boxes[i].y2,
                          min (native->drawable.height, MB_MAX_DIMENSION));

            if (x1 < x2 && y1 < y2)
                clipped[valid++] = (BoxRec){ x1, y1, x2, y2 };
        }
        input = RegionCreate (NULL, 0);
        if (input && !RegionInitBoxes (input, clipped, valid))
        {
            RegionDestroy (input);
            input = NULL;
        }
        free (clipped);
        if (!input)
            return FALSE;
    }
    if (!surface->input_default || wInputShape (native))
    {
        if (!MakeWindowOptional (native))
        {
            if (input)
                RegionDestroy (input);
            return FALSE;
        }
        if (native->optional->inputShape)
            RegionDestroy (native->optional->inputShape);
        native->optional->inputShape = input;
        native->drawable.pScreen->SetShape (native, ShapeInput);
        SendShapeNotify (native, ShapeInput);
    }
    return TRUE;
}

static void
surface_commit (struct wl_client *client, struct wl_resource *resource)
{
    MbSurface *surface = wl_resource_get_user_data (resource);
    WindowPtr native = mb_window_get (&surface->window, DixSetAttrAccess);

    if (!native)
        return;
    if (surface->input_changed)
    {
        if (!pixman_region32_copy (&surface->input, &surface->pending_input))
            goto fail;
        surface->input_default = surface->pending_input_default;
        surface->input_changed = FALSE;
    }
    if (!apply_input (surface, native))
        goto fail;
    if (surface->detach_pending)
    {
        UnmapWindow (native, FALSE);
        surface->detach_pending = FALSE;
    }
    wl_list_insert_list (surface->frames.prev, &surface->pending_frames);
    wl_list_init (&surface->pending_frames);
    return;

fail:
    wl_resource_post_no_memory (resource);
}

static const struct wl_surface_interface surface_implementation = {
    .destroy = surface_destroy,
    .attach = surface_attach,
    .damage = surface_damage,
    .frame = surface_frame,
    .set_opaque_region = surface_opaque,
    .set_input_region = surface_input,
    .commit = surface_commit,
};

static void
create_surface (struct wl_client *client, struct wl_resource *resource,
                uint32_t id)
{
    MbClient *owner = mb_client_for_wayland (client);
    MbSurface *surface = calloc (1, sizeof (*surface));

    if (!surface)
    {
        wl_resource_post_no_memory (resource);
        return;
    }
    surface->resource = wl_resource_create (client, &wl_surface_interface,
                                            MB_COMPOSITOR_VERSION, id);
    if (!surface->resource)
    {
        free (surface);
        wl_resource_post_no_memory (resource);
        return;
    }
    surface->client = owner;
    surface->input_default = surface->pending_input_default = TRUE;
    wl_list_init (&surface->pending_frames);
    wl_list_init (&surface->frames);
    pixman_region32_init (&surface->input);
    pixman_region32_init (&surface->pending_input);
    wl_list_insert (compositor.surfaces.prev, &surface->link);
    wl_resource_set_implementation (surface->resource, &surface_implementation,
                                    surface, surface_destroyed);
    if (!mb_window_create (&surface->window, owner, screenInfo.screens[0]))
    {
        wl_resource_destroy (surface->resource);
        wl_resource_post_no_memory (resource);
    }
}

static void
region_destroyed (struct wl_resource *resource)
{
    pixman_region32_t *region = wl_resource_get_user_data (resource);

    pixman_region32_fini (region);
    free (region);
}

static void
region_destroy (struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_destroy (resource);
}

static void
region_add (struct wl_client *client, struct wl_resource *resource, int32_t x,
            int32_t y, int32_t width, int32_t height)
{
    pixman_region32_t *region = wl_resource_get_user_data (resource);
    uint32_t clipped_width, clipped_height;

    if (width <= 0 || height <= 0)
        return;
    clipped_width = min ((int64_t)width, (int64_t)INT32_MAX - x);
    clipped_height = min ((int64_t)height, (int64_t)INT32_MAX - y);
    if (!pixman_region32_union_rect (region, region, x, y, clipped_width,
                                     clipped_height))
        wl_resource_post_no_memory (resource);
}

static void
region_subtract (struct wl_client *client, struct wl_resource *resource,
                 int32_t x, int32_t y, int32_t width, int32_t height)
{
    pixman_region32_t *region = wl_resource_get_user_data (resource);
    pixman_region32_t operand;
    Bool success;

    if (width <= 0 || height <= 0)
        return;
    pixman_region32_init_rect (&operand, x, y,
                               min ((int64_t)width, (int64_t)INT32_MAX - x),
                               min ((int64_t)height, (int64_t)INT32_MAX - y));
    success = pixman_region32_subtract (region, region, &operand);
    pixman_region32_fini (&operand);
    if (!success)
        wl_resource_post_no_memory (resource);
}

static const struct wl_region_interface region_implementation = {
    .destroy = region_destroy,
    .add = region_add,
    .subtract = region_subtract,
};

static void
create_region (struct wl_client *client, struct wl_resource *resource,
               uint32_t id)
{
    pixman_region32_t *region = calloc (1, sizeof (*region));
    struct wl_resource *result;

    if (!region)
    {
        wl_resource_post_no_memory (resource);
        return;
    }
    result = wl_resource_create (client, &wl_region_interface, 1, id);
    if (!result)
    {
        free (region);
        wl_resource_post_no_memory (resource);
        return;
    }
    pixman_region32_init (region);
    wl_resource_set_implementation (result, &region_implementation, region,
                                    region_destroyed);
}

static const struct wl_compositor_interface implementation = {
    .create_surface = create_surface,
    .create_region = create_region,
};

static void
bind_compositor (struct wl_client *client, void *data, uint32_t version,
                 uint32_t id)
{
    struct wl_resource *resource = wl_resource_create (
        client, &wl_compositor_interface, MB_COMPOSITOR_VERSION, id);

    if (!resource)
    {
        wl_client_post_no_memory (client);
        return;
    }
    wl_resource_set_implementation (resource, &implementation, NULL, NULL);
}

Bool
mb_surfaces_initialize (void)
{
    wl_list_init (&compositor.surfaces);
    compositor.surface_global =
        wl_global_create (compositor.wl_display, &wl_compositor_interface,
                          MB_COMPOSITOR_VERSION, NULL, bind_compositor);
    return compositor.surface_global != NULL;
}

void
mb_surfaces_shutdown (void)
{
    if (compositor.surface_global)
    {
        wl_global_destroy (compositor.surface_global);
        compositor.surface_global = NULL;
    }
}
