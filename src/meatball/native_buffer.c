/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "server.h"
#include "single-pixel-buffer.h"

#include <gcstruct.h>
#include <pixmapstr.h>
#include <servermd.h>
#include <stdlib.h>
#include <wayland-server-protocol.h>

#define MB_ALL_PLANES (~0UL)
#define SINGLE_PIXEL_VERSION 1
#define RENDER_CHANNEL_SHIFT 16

void
mb_buffer_unref (MbBuffer *buffer)
{
    if (--buffer->refs)
        return;
    if (buffer->destroy)
        buffer->destroy (buffer);
    else
        free (buffer);
}

static void
buffer_destroyed (struct wl_resource *resource)
{
    MbBuffer *buffer = wl_resource_get_user_data (resource);

    buffer->resource = NULL;
    mb_buffer_unref (buffer);
}

static void
destroy_resource (struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_destroy (resource);
}

static const struct wl_buffer_interface implementation = {
    .destroy = destroy_resource,
};

Bool
mb_buffer_publish (MbBuffer *buffer, struct wl_client *client, uint32_t id)
{
    buffer->resource = wl_resource_create (
        client, &wl_buffer_interface, WL_BUFFER_DESTROY_SINCE_VERSION, id);
    if (!buffer->resource)
        return FALSE;
    buffer->refs = 1;
    wl_resource_set_implementation (buffer->resource, &implementation, buffer,
                                    buffer_destroyed);
    return TRUE;
}

MbBuffer *
mb_buffer_acquire (struct wl_resource *resource)
{
    MbBuffer *buffer;

    if (!resource || !wl_resource_instance_of (resource, &wl_buffer_interface,
                                               &implementation))
        return NULL;
    buffer = wl_resource_get_user_data (resource);
    buffer->refs++;
    return buffer;
}

static void
create_single_pixel (struct wl_client *client, struct wl_resource *resource,
                     uint32_t id, uint32_t r, uint32_t g, uint32_t b,
                     uint32_t a)
{
    MbBuffer *buffer = calloc (1, sizeof (*buffer));

    if (!buffer)
    {
        wl_resource_post_no_memory (resource);
        return;
    }
    *buffer = (MbBuffer){
        .width = 1,
        .height = 1,
        .solid = TRUE,
        .solid_color = { .red = r >> RENDER_CHANNEL_SHIFT,
                         .green = g >> RENDER_CHANNEL_SHIFT,
                         .blue = b >> RENDER_CHANNEL_SHIFT,
                         .alpha = a >> RENDER_CHANNEL_SHIFT },
    };
    if (!mb_buffer_publish (buffer, client, id))
    {
        free (buffer);
        wl_resource_post_no_memory (resource);
    }
}

static const struct wp_single_pixel_buffer_manager_v1_interface
    single_pixel_implementation = {
        .destroy = destroy_resource,
        .create_u32_rgba_buffer = create_single_pixel,
    };

static void
bind_single_pixel (struct wl_client *client, void *data, uint32_t version,
                   uint32_t id)
{
    struct wl_resource *resource = wl_resource_create (
        client, &wp_single_pixel_buffer_manager_v1_interface, version, id);

    if (!resource)
    {
        wl_client_post_no_memory (client);
        return;
    }
    wl_resource_set_implementation (resource, &single_pixel_implementation,
                                    NULL, NULL);
}

Bool
mb_single_pixel_initialize (void)
{
    compositor.single_pixel_global = wl_global_create (
        compositor.wl_display, &wp_single_pixel_buffer_manager_v1_interface,
        SINGLE_PIXEL_VERSION, NULL, bind_single_pixel);
    return compositor.single_pixel_global != NULL;
}

void
mb_single_pixel_shutdown (void)
{
    if (compositor.single_pixel_global)
    {
        wl_global_destroy (compositor.single_pixel_global);
        compositor.single_pixel_global = NULL;
    }
}

PixmapPtr
mb_pixmap_upload (ScreenPtr screen, unsigned int width, unsigned int height,
                  unsigned int depth, char *pixels)
{
    PixmapPtr copy = screen->CreatePixmap (screen, width, height, depth, 0);
    GCPtr gc;

    if (!copy)
        return NULL;
    gc = GetScratchGC (depth, screen);
    if (!gc)
    {
        screen->DestroyPixmap (copy);
        return NULL;
    }
    ValidateGC (&copy->drawable, gc);
    gc->ops->PutImage (&copy->drawable, gc, depth, 0, 0, width, height, 0,
                       ZPixmap, pixels);
    FreeScratchGC (gc);
    return copy;
}

PixmapPtr
mb_pixmap_snapshot (PixmapPtr imported)
{
    ScreenPtr screen = imported->drawable.pScreen;
    unsigned int depth = imported->drawable.depth;
    unsigned int width = imported->drawable.width;
    unsigned int height = imported->drawable.height;
    size_t stride = PixmapBytePad (width, depth);
    size_t size = stride * height;
    PixmapPtr copy;
    char *pixels;

    if (size > MB_MAX_SNAPSHOT_BYTES)
        return NULL;
    pixels = malloc (size);
    if (!pixels)
        return NULL;
    screen->GetImage (&imported->drawable, 0, 0, width, height, ZPixmap,
                      MB_ALL_PLANES, pixels);
    copy = mb_pixmap_upload (screen, width, height, depth, pixels);
    free (pixels);
    return copy;
}
