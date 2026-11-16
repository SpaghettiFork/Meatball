/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "server.h"

#include <X11/extensions/shapeconst.h>
#include <limits.h>
#include <pixmapstr.h>
#include <stdlib.h>
#include <wayland-server-protocol.h>
#include <window.h>

#define FRAME_DELAY_MS 16
#define SEND_EVENT_MASK 0x80
#define SURFACE_COMMIT_LIMIT 256

static Bool apply_input (MbSurface *surface, WindowPtr native);

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
content_unref (MbSurface *surface, MbSurfaceContent *content)
{
    PixmapPtr pixels;

    if (!content || --content->refs)
        return;
    if (content->pixels && surface->client->native &&
        dixLookupResourceByType ((void **)&pixels, content->pixels, RT_PIXMAP,
                                 surface->client->native,
                                 DixDestroyAccess) == Success)
        FreeResource (content->pixels, RT_NONE);
    mb_shm_buffer_unref (content->buffer);
    free (content);
}

void
mb_surface_commit_free (MbSurfaceCommit *commit)
{
    struct Frame *frame, *next;

    wl_list_for_each_safe (frame, next, &commit->frames, link)
    {
        wl_resource_destroy (frame->resource);
    }
    content_unref (commit->surface, commit->content);
    mb_subsurface_commit_free (commit);
    pixman_region32_fini (&commit->input);
    if (!wl_list_empty (&commit->link))
        commit->surface->commit_count--;
    wl_list_remove (&commit->link);
    free (commit);
}

static int
frame_ready (void *data)
{
    MbSurface *surface = data;
    struct Frame *frame, *next;
    WindowPtr window = mb_window_get (&surface->window, DixGetAttrAccess);
    MbSceneIterator iterator = { .root = surface, .surface = surface };
    MbSurface *node;

    surface->frame_timer_armed = FALSE;
    if (!window || (!window->realized && !mb_input_cursor_visible (surface)) ||
        !surface->pixels || mb_subsurface_root (surface) != surface)
        return 0;
    while ((node = mb_scene_next (&iterator)))
    {
        wl_list_for_each_safe (frame, next, &node->frames, link)
        {
            wl_callback_send_done (frame->resource, GetTimeInMillis ());
            wl_resource_destroy (frame->resource);
        }
    }
    return 0;
}

static Bool
redraw (MbSurface *surface)
{
    WindowPtr window;
    MbSceneIterator iterator;
    MbSurface *node;
    Bool frames = FALSE;

    surface = mb_subsurface_root (surface);
    if (!surface)
        return TRUE;
    window = mb_window_get (&surface->window, DixGetAttrAccess);

    if (!window || (!window->realized && !mb_input_cursor_visible (surface)) ||
        !surface->pixels)
        return TRUE;
    if (!mb_surface_redraw (surface))
        return FALSE;
    iterator = (MbSceneIterator){ .root = surface, .surface = surface };
    while ((node = mb_scene_next (&iterator)))
        if (!wl_list_empty (&node->frames))
            frames = TRUE;
    if (!surface->frame_timer_armed && frames)
    {
        if (!surface->frame_timer)
            surface->frame_timer = wl_event_loop_add_timer (
                compositor.wl_event_loop, frame_ready, surface);
        if (!surface->frame_timer ||
            wl_event_source_timer_update (surface->frame_timer,
                                          FRAME_DELAY_MS) < 0)
            return FALSE;
        surface->frame_timer_armed = TRUE;
    }
    return TRUE;
}

static void
surface_output (MbClient *client, const void *message, size_t size)
{
    const xEvent *event = message;
    MbSurface *surface;
    XID window;

    if (size != sizeof (*event))
        return;
    switch (event->u.u.type & ~SEND_EVENT_MASK)
    {
    case Expose:
        if (event->u.expose.count)
            return;
        window = event->u.expose.window;
        break;
    case ConfigureNotify:
        window = event->u.configureNotify.window;
        break;
    case ClientMessage:
        window = event->u.clientMessage.window;
        break;
    case PropertyNotify:
        window = event->u.property.window;
        break;
    default:
        return;
    }
    wl_list_for_each (surface, &compositor.surfaces, link)
    {
        if (surface->client == client && surface->window.id == window)
        {
            WindowPtr native =
                mb_window_get (&surface->window, DixSetAttrAccess);

            mb_xdg_event (surface, event);
            if (native && !apply_input (surface, native))
            {
                wl_resource_post_no_memory (surface->resource);
                return;
            }
            if (!redraw (surface))
                wl_resource_post_no_memory (surface->resource);
            return;
        }
    }
}

static void
surface_destroyed (struct wl_resource *resource)
{
    MbSurface *surface = wl_resource_get_user_data (resource);
    struct Frame *frame, *next;
    MbSurfaceCommit *commit, *next_commit;

    mb_data_surface_destroyed (surface);
    mb_outputs_surface_destroyed (surface);
    mb_input_surface_destroyed (surface);
    mb_subsurface_surface_destroyed (surface);
    mb_xdg_surface_destroyed (surface);
    mb_viewport_surface_destroyed (surface);
    if (surface->frame_timer)
        wl_event_source_remove (surface->frame_timer);
    if (surface->pending_buffer)
        mb_shm_buffer_unref (surface->pending_buffer);
    wl_list_for_each_safe (commit, next_commit, &surface->commits, link)
    {
        mb_surface_commit_free (commit);
    }
    content_unref (surface, surface->content);
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
    MbSurface *surface = wl_resource_get_user_data (resource);

    if (!mb_xdg_surface_destroy_request (surface))
        return;
    wl_resource_destroy (resource);
}

static void
surface_attach (struct wl_client *client, struct wl_resource *resource,
                struct wl_resource *buffer, int32_t x, int32_t y)
{
    MbSurface *surface = wl_resource_get_user_data (resource);

    MbShmBuffer *shm = buffer ? mb_shm_buffer_acquire (buffer) : NULL;

    if (buffer && !shm)
    {
        wl_client_post_implementation_error (
            client, "unsupported buffer implementation");
        return;
    }
    if (surface->pending_buffer)
        mb_shm_buffer_unref (surface->pending_buffer);
    surface->pending_buffer = shm;
    surface->pending_x = x;
    surface->pending_y = y;
    surface->attach_pending = TRUE;
}

static void
surface_damage (struct wl_client *client, struct wl_resource *resource,
                int32_t x, int32_t y, int32_t width, int32_t height)
{
    /* Commits redraw the full surface from its server-owned snapshot. */
}

static void
surface_transform (struct wl_client *client, struct wl_resource *resource,
                   int32_t transform)
{
    MbSurface *surface = wl_resource_get_user_data (resource);

    if (transform < WL_OUTPUT_TRANSFORM_NORMAL ||
        transform > WL_OUTPUT_TRANSFORM_FLIPPED_270)
    {
        wl_resource_post_error (resource, WL_SURFACE_ERROR_INVALID_TRANSFORM,
                                "invalid buffer transform");
        return;
    }
    surface->pending_transform = transform;
}

static void
surface_scale (struct wl_client *client, struct wl_resource *resource,
               int32_t scale)
{
    MbSurface *surface = wl_resource_get_user_data (resource);

    if (scale <= 0)
    {
        wl_resource_post_error (resource, WL_SURFACE_ERROR_INVALID_SCALE,
                                "buffer scale must be positive");
        return;
    }
    surface->pending_scale = scale;
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
    pixman_region32_t aggregate;
    pixman_region32_t *area = &surface->input;
    Bool input_default = surface->input_default;
    Bool success = FALSE;

    pixman_region32_init (&aggregate);
    if (surface->role == MB_ROLE_DRAG_ICON)
    {
        input_default = FALSE;
        area = &aggregate;
    }
    else if (surface->pixels && surface->stack_count > 1)
    {
        MbSceneIterator iterator = { .root = surface, .surface = surface };
        MbSurface *node;

        input_default = FALSE;
        area = &aggregate;
        while ((node = mb_scene_next (&iterator)))
        {
            pixman_region32_t region;
            Bool copied;

            pixman_region32_init_rect (&region, 0, 0, node->width,
                                       node->height);
            copied =
                node->input_default ||
                pixman_region32_intersect (&region, &region, &node->input);
            if (copied)
            {
                pixman_region32_translate (&region,
                                           iterator.x - surface->scene_x,
                                           iterator.y - surface->scene_y);
                copied =
                    pixman_region32_union (&aggregate, &aggregate, &region);
            }
            pixman_region32_fini (&region);
            if (!copied)
                goto out;
        }
        if (iterator.failed)
            goto out;
    }

    if (!input_default)
    {
        int count, i, valid = 0;
        pixman_box32_t *boxes = pixman_region32_rectangles (area, &count);
        BoxPtr clipped = count ? calloc (count, sizeof (*clipped)) : NULL;

        if (count && !clipped)
            goto out;
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
            goto out;
    }
    if ((input || wInputShape (native)) &&
        !(input && wInputShape (native) &&
          RegionEqual (input, wInputShape (native))))
    {
        if (!MakeWindowOptional (native))
        {
            goto out;
        }
        if (native->optional->inputShape)
            RegionDestroy (native->optional->inputShape);
        native->optional->inputShape = input;
        input = NULL;
        native->drawable.pScreen->SetShape (native, ShapeInput);
        SendShapeNotify (native, ShapeInput);
    }
    success = TRUE;

out:
    if (input)
        RegionDestroy (input);
    pixman_region32_fini (&aggregate);
    return success;
}

static MbSurfaceCommit *
capture_commit (MbSurface *surface)
{
    MbSurfaceCommit *previous =
        wl_list_empty (&surface->commits)
            ? NULL
            : wl_container_of (surface->commits.prev, previous, link);
    MbSurfaceContent *content =
        previous ? previous->content : surface->content;
    MbShmBuffer *buffer = content ? content->buffer : NULL;
    pixman_region32_t *input = previous ? &previous->input : &surface->input;
    MbSurfaceCommit *commit;
    int32_t width = 0, height = 0;

    if (surface->commit_count >= SURFACE_COMMIT_LIMIT)
    {
        wl_client_post_implementation_error (
            wl_resource_get_client (surface->resource),
            "too many queued surface commits");
        return NULL;
    }
    if (surface->attach_pending)
        buffer = surface->pending_buffer;
    if (buffer)
    {
        Bool rotated =
            surface->pending_transform == WL_OUTPUT_TRANSFORM_90 ||
            surface->pending_transform == WL_OUTPUT_TRANSFORM_270 ||
            surface->pending_transform == WL_OUTPUT_TRANSFORM_FLIPPED_90 ||
            surface->pending_transform == WL_OUTPUT_TRANSFORM_FLIPPED_270;

        if (buffer->width % surface->pending_scale ||
            buffer->height % surface->pending_scale)
        {
            wl_resource_post_error (
                surface->resource, WL_SURFACE_ERROR_INVALID_SIZE,
                "buffer dimensions must be multiples of scale");
            return NULL;
        }
        width = rotated ? buffer->height : buffer->width;
        height = rotated ? buffer->width : buffer->height;
    }

    if (!mb_viewport_size (surface, width, height, &width, &height) ||
        !mb_xdg_commit (surface))
        return NULL;
    commit = calloc (1, sizeof (*commit));
    if (!commit)
    {
        wl_resource_post_no_memory (surface->resource);
        return NULL;
    }
    commit->surface = surface;
    wl_list_init (&commit->link);
    wl_list_init (&commit->dependency_link);
    wl_list_init (&commit->dependencies);
    wl_list_init (&commit->frames);
    pixman_region32_init (&commit->input);
    if (surface->input_changed)
        input = &surface->pending_input;
    if (!pixman_region32_copy (&commit->input, input))
        goto fail;
    commit->input_default =
        previous ? previous->input_default : surface->input_default;
    if (surface->input_changed)
        commit->input_default = surface->pending_input_default;
    commit->crop = surface->pending_crop;
    commit->scale = surface->pending_scale;
    commit->transform = surface->pending_transform;
    commit->width = width;
    commit->height = height;
    if (!mb_subsurface_capture (commit))
        goto fail;
    if (surface->attach_pending)
    {
        if (buffer)
        {
            PixmapPtr copy;
            XID id;

            commit->content = calloc (1, sizeof (*commit->content));
            if (!commit->content)
                goto fail;
            commit->content->refs = 1;
            commit->content->buffer = buffer;
            buffer->refs++;
            copy = mb_shm_snapshot (buffer, surface->window.screen);
            if (!copy)
                goto fail;
            id = FakeClientID (surface->client->native->index);
            copy->drawable.id = id;
            if (!AddResource (id, RT_PIXMAP, copy))
                goto fail;
            commit->content->pixels = id;
            commit->offset_x = surface->pending_x;
            commit->offset_y = surface->pending_y;
            if (buffer->resource)
                wl_buffer_send_release (buffer->resource);
        }
        if (surface->pending_buffer)
            mb_shm_buffer_unref (surface->pending_buffer);
        surface->pending_buffer = NULL;
        surface->attach_pending = FALSE;
    }
    else if (content)
    {
        content->refs++;
        commit->content = content;
    }
    surface->input_changed = FALSE;
    wl_list_insert_list (commit->frames.prev, &surface->pending_frames);
    wl_list_init (&surface->pending_frames);
    wl_list_insert (surface->commits.prev, &commit->link);
    surface->commit_count++;
    return commit;

fail:
    mb_surface_commit_free (commit);
    wl_resource_post_no_memory (surface->resource);
    return NULL;
}

void
mb_surface_commit_apply (MbSurfaceCommit *commit)
{
    MbSurface *surface = commit->surface;

    mb_subsurface_apply (commit);
    content_unref (surface, surface->content);
    surface->content = commit->content;
    commit->content = NULL;
    surface->buffer = surface->content ? surface->content->buffer : NULL;
    surface->pixels = surface->content ? surface->content->pixels : None;
    pixman_region32_fini (&surface->input);
    surface->input = commit->input;
    pixman_region32_init (&commit->input);
    surface->input_default = commit->input_default;
    surface->crop = commit->crop;
    surface->scale = commit->scale;
    surface->transform = commit->transform;
    surface->width = commit->width;
    surface->height = commit->height;
    if (surface->role != MB_ROLE_SUBSURFACE)
    {
        surface->offset_x = commit->offset_x;
        surface->offset_y = commit->offset_y;
    }
    wl_list_insert_list (surface->frames.prev, &commit->frames);
    wl_list_init (&commit->frames);
}

Bool
mb_surface_update (MbSurface *surface)
{
    WindowPtr native = mb_window_get (&surface->window, DixSetAttrAccess);
    MbSceneIterator iterator = { .root = surface, .surface = surface };
    MbSurface *node;
    int64_t x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    Bool offset;

    if (!native)
        return TRUE;
    while ((node = mb_scene_next (&iterator)))
    {
        x1 = min (x1, iterator.x);
        y1 = min (y1, iterator.y);
        x2 = max (x2, iterator.x + node->width);
        y2 = max (y2, iterator.y + node->height);
    }
    if (iterator.failed || x2 - x1 > MB_MAX_DIMENSION ||
        y2 - y1 > MB_MAX_DIMENSION ||
        (x2 - x1) * (y2 - y1) >
            MB_SHM_MAX_SNAPSHOT_BYTES / MB_SHM_BYTES_PER_PIXEL)
    {
        wl_resource_post_error (surface->resource,
                                WL_SURFACE_ERROR_INVALID_SIZE,
                                "surface tree exceeds native canvas limits");
        return FALSE;
    }
    offset = surface->offset_x || surface->offset_y ||
             surface->scene_x != x1 || surface->scene_y != y1;
    if (!surface->pixels)
    {
        surface->scene_width = surface->scene_height = 0;
        surface->scene_x = surface->scene_y = 0;
        UnmapWindow (native, FALSE);
    }
    else if (offset || surface->scene_width != x2 - x1 ||
             surface->scene_height != y2 - y1)
    {
        XID values[4];
        Mask mask = CWWidth | CWHeight;
        unsigned int count = 0;

        if (offset && surface->role != MB_ROLE_DRAG_ICON)
        {
            int64_t x = (int64_t)native->origin.x + surface->offset_x + x1 -
                        surface->scene_x;
            int64_t y = (int64_t)native->origin.y + surface->offset_y + y1 -
                        surface->scene_y;

            if (x < INT16_MIN || x > INT16_MAX || y < INT16_MIN ||
                y > INT16_MAX)
            {
                wl_resource_post_error (
                    surface->resource, WL_SURFACE_ERROR_INVALID_SIZE,
                    "buffer offset exceeds native coordinates");
                return FALSE;
            }
            values[count++] = (INT32)x;
            values[count++] = (INT32)y;
            mask |= CWX | CWY;
        }
        values[count++] = x2 - x1;
        values[count++] = y2 - y1;

        if (ConfigureWindow (native, mask, values, surface->client->native) !=
            Success)
            goto fail;
        surface->scene_width = x2 - x1;
        surface->scene_height = y2 - y1;
        surface->scene_x = x1;
        surface->scene_y = y1;
    }
    if (!mb_input_cursor_changed (surface))
        goto fail;
    if (!apply_input (surface, native) || !mb_data_icon_changed (surface))
        goto fail;
    surface->offset_x = surface->offset_y = 0;
    if (!mb_xdg_map (surface) || !redraw (surface))
        goto fail;
    return TRUE;

fail:
    wl_resource_post_no_memory (surface->resource);
    return FALSE;
}

static void
surface_commit (struct wl_client *client, struct wl_resource *resource)
{
    MbSurface *surface = wl_resource_get_user_data (resource);
    MbSurfaceCommit *commit;

    if (!mb_window_get (&surface->window, DixSetAttrAccess))
        return;
    commit = capture_commit (surface);
    if (!commit)
        return;
    mb_subsurface_commit (commit);
}

static const struct wl_surface_interface surface_implementation = {
    .destroy = surface_destroy,
    .attach = surface_attach,
    .damage = surface_damage,
    .frame = surface_frame,
    .set_opaque_region = surface_opaque,
    .set_input_region = surface_input,
    .commit = surface_commit,
    .set_buffer_transform = surface_transform,
    .set_buffer_scale = surface_scale,
    .damage_buffer = surface_damage,
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
    surface->resource = wl_resource_create (
        client, &wl_surface_interface, wl_resource_get_version (resource), id);
    if (!surface->resource)
    {
        free (surface);
        wl_resource_post_no_memory (resource);
        return;
    }
    surface->client = owner;
    surface->scale = surface->pending_scale = 1;
    surface->input_default = surface->pending_input_default = TRUE;
    wl_list_init (&surface->pending_frames);
    wl_list_init (&surface->frames);
    wl_list_init (&surface->commits);
    wl_list_init (&surface->pending_stack);
    wl_list_insert (&surface->pending_stack, &surface->pending_self);
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
    else
        owner->output = surface_output;
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
    struct wl_resource *resource =
        wl_resource_create (client, &wl_compositor_interface, version, id);

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
