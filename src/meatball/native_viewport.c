/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "server.h"

#include "viewporter.h"
#include <stdlib.h>
#include <wayland-server-protocol.h>

#define VIEWPORT_VERSION 1
#define WAYLAND_FIXED_ONE 256
#define VIEWPORT_UNSET (-WAYLAND_FIXED_ONE)
#define VIEWPORT_DESTINATION_UNSET (-1)

struct MbViewport
{
    struct wl_resource *resource;
    MbSurface *surface;
};

static void
viewport_destroyed (struct wl_resource *resource)
{
    struct MbViewport *viewport = wl_resource_get_user_data (resource);

    if (viewport->surface)
    {
        viewport->surface->pending_crop = (MbViewportState){ 0 };
        viewport->surface->viewport = NULL;
    }
    free (viewport);
}

static void
destroy (struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_destroy (resource);
}

static void
set_source (struct wl_client *client, struct wl_resource *resource,
            wl_fixed_t x, wl_fixed_t y, wl_fixed_t width, wl_fixed_t height)
{
    struct MbViewport *viewport = wl_resource_get_user_data (resource);
    MbViewportState *state;

    if (!viewport->surface)
    {
        wl_resource_post_error (resource, WP_VIEWPORT_ERROR_NO_SURFACE,
                                "viewport surface has been destroyed");
        return;
    }
    state = &viewport->surface->pending_crop;
    if (x == VIEWPORT_UNSET && y == VIEWPORT_UNSET &&
        width == VIEWPORT_UNSET && height == VIEWPORT_UNSET)
    {
        state->source_set = FALSE;
        return;
    }
    if (x < 0 || y < 0 || width <= 0 || height <= 0)
    {
        wl_resource_post_error (resource, WP_VIEWPORT_ERROR_BAD_VALUE,
                                "invalid viewport source rectangle");
        return;
    }
    state->x = x;
    state->y = y;
    state->width = width;
    state->height = height;
    state->source_set = TRUE;
}

static void
set_destination (struct wl_client *client, struct wl_resource *resource,
                 int32_t width, int32_t height)
{
    struct MbViewport *viewport = wl_resource_get_user_data (resource);
    MbViewportState *state;

    if (!viewport->surface)
    {
        wl_resource_post_error (resource, WP_VIEWPORT_ERROR_NO_SURFACE,
                                "viewport surface has been destroyed");
        return;
    }
    state = &viewport->surface->pending_crop;
    if (width == VIEWPORT_DESTINATION_UNSET &&
        height == VIEWPORT_DESTINATION_UNSET)
    {
        state->destination_set = FALSE;
        return;
    }
    if (width <= 0 || height <= 0)
    {
        wl_resource_post_error (resource, WP_VIEWPORT_ERROR_BAD_VALUE,
                                "viewport destination must be positive");
        return;
    }
    state->destination_width = width;
    state->destination_height = height;
    state->destination_set = TRUE;
}

static const struct wp_viewport_interface viewport_implementation = {
    .destroy = destroy,
    .set_source = set_source,
    .set_destination = set_destination,
};

static void
get_viewport (struct wl_client *client, struct wl_resource *resource,
              uint32_t id, struct wl_resource *surface_resource)
{
    MbSurface *surface = wl_resource_get_user_data (surface_resource);
    struct MbViewport *viewport;

    if (surface->viewport)
    {
        wl_resource_post_error (resource, WP_VIEWPORTER_ERROR_VIEWPORT_EXISTS,
                                "surface already has a viewport");
        return;
    }
    viewport = calloc (1, sizeof (*viewport));
    if (!viewport)
    {
        wl_resource_post_no_memory (resource);
        return;
    }
    viewport->resource = wl_resource_create (client, &wp_viewport_interface,
                                             VIEWPORT_VERSION, id);
    if (!viewport->resource)
    {
        free (viewport);
        wl_resource_post_no_memory (resource);
        return;
    }
    viewport->surface = surface;
    surface->viewport = viewport;
    wl_resource_set_implementation (viewport->resource,
                                    &viewport_implementation, viewport,
                                    viewport_destroyed);
}

static const struct wp_viewporter_interface implementation = {
    .destroy = destroy,
    .get_viewport = get_viewport,
};

static void
bind_viewporter (struct wl_client *client, void *data, uint32_t version,
                 uint32_t id)
{
    struct wl_resource *resource =
        wl_resource_create (client, &wp_viewporter_interface, version, id);

    if (!resource)
    {
        wl_client_post_no_memory (client);
        return;
    }
    wl_resource_set_implementation (resource, &implementation, NULL, NULL);
}

Bool
mb_viewport_size (MbSurface *surface, int32_t buffer_width,
                  int32_t buffer_height, int32_t *width, int32_t *height)
{
    MbViewportState *state = &surface->pending_crop;

    if (state->source_set && !state->destination_set &&
        (state->width % WAYLAND_FIXED_ONE ||
         state->height % WAYLAND_FIXED_ONE))
    {
        wl_resource_post_error (
            surface->viewport->resource, WP_VIEWPORT_ERROR_BAD_SIZE,
            "fractional source size requires a destination");
        return FALSE;
    }
    if (!buffer_width || !buffer_height)
    {
        *width = *height = 0;
        return TRUE;
    }
    if (state->source_set &&
        (((int64_t)state->x + state->width) * surface->pending_scale >
             (int64_t)buffer_width * WAYLAND_FIXED_ONE ||
         ((int64_t)state->y + state->height) * surface->pending_scale >
             (int64_t)buffer_height * WAYLAND_FIXED_ONE))
    {
        wl_resource_post_error (surface->viewport->resource,
                                WP_VIEWPORT_ERROR_OUT_OF_BUFFER,
                                "viewport source extends outside the buffer");
        return FALSE;
    }
    if (state->destination_set)
    {
        *width = state->destination_width;
        *height = state->destination_height;
    }
    else if (state->source_set)
    {
        *width = wl_fixed_to_int (state->width);
        *height = wl_fixed_to_int (state->height);
    }
    else
    {
        *width = buffer_width / surface->pending_scale;
        *height = buffer_height / surface->pending_scale;
    }
    if (*width > MB_MAX_DIMENSION || *height > MB_MAX_DIMENSION)
    {
        wl_resource_post_error (surface->resource,
                                WL_SURFACE_ERROR_INVALID_SIZE,
                                "surface dimensions exceed native limits");
        return FALSE;
    }
    return TRUE;
}

void
mb_viewport_surface_destroyed (MbSurface *surface)
{
    if (surface->viewport)
    {
        surface->viewport->surface = NULL;
        surface->viewport = NULL;
    }
}

Bool
mb_viewport_initialize (void)
{
    compositor.viewport_global =
        wl_global_create (compositor.wl_display, &wp_viewporter_interface,
                          VIEWPORT_VERSION, NULL, bind_viewporter);
    return compositor.viewport_global != NULL;
}

void
mb_viewport_shutdown (void)
{
    if (compositor.viewport_global)
    {
        wl_global_destroy (compositor.viewport_global);
        compositor.viewport_global = NULL;
    }
}
