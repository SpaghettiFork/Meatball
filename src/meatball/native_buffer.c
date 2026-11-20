/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "native_buffer.h"

#include <gcstruct.h>
#include <pixmapstr.h>
#include <servermd.h>
#include <stdlib.h>
#include <wayland-server-protocol.h>

#define MB_ALL_PLANES (~0UL)

void
mb_buffer_unref (MbBuffer *buffer)
{
    if (!--buffer->refs)
        buffer->destroy (buffer);
}

static void
buffer_destroyed (struct wl_resource *resource)
{
    MbBuffer *buffer = wl_resource_get_user_data (resource);

    buffer->resource = NULL;
    mb_buffer_unref (buffer);
}

static void
buffer_destroy (struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_destroy (resource);
}

static const struct wl_buffer_interface implementation = {
    .destroy = buffer_destroy,
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
