/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "server.h"

#ifdef DRI3
#include "linux-dmabuf.h"

#include <dri3.h>
#include <drm_fourcc.h>
#include <extnsionst.h>
#include <limits.h>
#include <picturestr.h>
#include <pixmapstr.h>
#include <stdlib.h>
#include <unistd.h>
#include <wayland-server-protocol.h>

#define DMABUF_VERSION ZWP_LINUX_DMABUF_V1_MODIFIER_SINCE_VERSION
#define DMABUF_MAX_PLANES 4
#define MODIFIER_HALF_BITS 32
#define DMABUF_BPP 32
#define DMABUF_BYTES_PER_PIXEL (DMABUF_BPP / CHAR_BIT)

typedef struct
{
    uint32_t fourcc;
    unsigned int depth;
    CARD32 render_format;
} Format;

static const Format formats[] = {
    { DRM_FORMAT_XRGB8888, MB_SHM_XRGB_DEPTH, PICT_x8r8g8b8 },
    { DRM_FORMAT_ARGB8888, MB_SHM_ARGB_DEPTH, PICT_a8r8g8b8 },
};

typedef struct
{
    CARD32 count;
    CARD64 *values;
} Modifiers;

struct MbDmabuf
{
    struct wl_global *global;
    ScreenPtr screen;
    Modifiers modifiers[ARRAY_SIZE (formats)];
};

typedef struct
{
    int fds[DMABUF_MAX_PLANES];
    CARD32 strides[DMABUF_MAX_PLANES], offsets[DMABUF_MAX_PLANES];
    uint64_t modifier;
    unsigned int mask;
    Bool used;
} Params;

typedef struct
{
    MbBuffer base;
    PixmapPtr pixmap;
    Bool y_invert;
} DmabufBuffer;

static void
dmabuf_buffer_destroy (MbBuffer *base)
{
    DmabufBuffer *buffer = (DmabufBuffer *)base;

    buffer->pixmap->drawable.pScreen->DestroyPixmap (buffer->pixmap);
    free (buffer);
}

static PixmapPtr
dmabuf_snapshot (MbBuffer *base, ScreenPtr screen)
{
    DmabufBuffer *buffer = (DmabufBuffer *)base;

    if (buffer->pixmap->drawable.pScreen != screen)
        return NULL;
    return mb_pixmap_snapshot (buffer->pixmap, buffer->y_invert);
}

static void
destroy (struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_destroy (resource);
}

static void
close_planes (Params *params)
{
    for (unsigned int i = 0; i < DMABUF_MAX_PLANES; i++)
    {
        if (params->mask & (1U << i))
            close (params->fds[i]);
    }
    params->mask = 0;
}

static void
params_destroyed (struct wl_resource *resource)
{
    Params *params = wl_resource_get_user_data (resource);

    close_planes (params);
    free (params);
}

static void
add_plane (struct wl_client *client, struct wl_resource *resource, int32_t fd,
           uint32_t index, uint32_t offset, uint32_t stride, uint32_t high,
           uint32_t low)
{
    Params *params = wl_resource_get_user_data (resource);
    uint64_t modifier = ((uint64_t)high << MODIFIER_HALF_BITS) | low;
    uint32_t error;
    const char *message;

    if (params->used)
    {
        error = ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED;
        message = "DMA-BUF parameters have already been used";
    }
    else if (index >= DMABUF_MAX_PLANES)
    {
        error = ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX;
        message = "DMA-BUF plane index is out of range";
    }
    else if (params->mask & (1U << index))
    {
        error = ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_SET;
        message = "DMA-BUF plane is already set";
    }
    else if (params->mask && modifier != params->modifier)
    {
        error = ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT;
        message = "DMA-BUF planes must share a modifier";
    }
    else
    {
        params->modifier = modifier;
        params->mask |= 1U << index;
        params->fds[index] = fd;
        params->offsets[index] = offset;
        params->strides[index] = stride;
        return;
    }
    close (fd);
    wl_resource_post_error (resource, error, "%s", message);
}

static void
create_buffer (struct wl_client *client, struct wl_resource *resource,
               uint32_t id, int32_t width, int32_t height, uint32_t fourcc,
               uint32_t flags)
{
    Params *params = wl_resource_get_user_data (resource);
    struct MbDmabuf *dmabuf = compositor.dmabuf;
    const Format *format = NULL;
    unsigned int count = 0;
    PixmapPtr pixmap = NULL;
    DmabufBuffer *buffer;
    Bool linear = params->modifier == DRM_FORMAT_MOD_LINEAR;
    Bool implicit = params->modifier == DRM_FORMAT_MOD_INVALID;
    uint32_t error;
    const char *message;

    if (params->used)
    {
        wl_resource_post_error (resource,
                                ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
                                "DMA-BUF parameters have already been used");
        return;
    }
    params->used = TRUE;
    if (width <= 0 || height <= 0 || width > MB_MAX_DIMENSION ||
        height > MB_MAX_DIMENSION ||
        (int64_t)width * height * DMABUF_BYTES_PER_PIXEL >
            MB_MAX_SNAPSHOT_BYTES)
    {
        error = ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_DIMENSIONS;
        message = "invalid DMA-BUF dimensions";
        goto invalid;
    }
    while (count < DMABUF_MAX_PLANES && (params->mask & (1U << count)))
        count++;
    if (!count || params->mask != (1U << count) - 1 ||
        ((linear || implicit) && count != 1))
    {
        error = ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE;
        message = "invalid DMA-BUF plane set";
        goto invalid;
    }
    for (size_t i = 0; i < ARRAY_SIZE (formats); i++)
    {
        if (formats[i].fourcc != fourcc)
            continue;
        for (CARD32 m = 0; m < dmabuf->modifiers[i].count; m++)
        {
            if (dmabuf->modifiers[i].values[m] == params->modifier)
            {
                format = &formats[i];
                break;
            }
        }
        break;
    }
    if (!format)
    {
        /* Older clients may attempt an implicit modifier from a format
         * event, or try a pair without prior version 3 advertisement. */
        goto failed;
    }
    if (flags & ~ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_Y_INVERT)
        goto failed;
    for (unsigned int i = 0; i < count; i++)
    {
        uint64_t offset = params->offsets[i];
        uint64_t stride = params->strides[i];
        uint64_t end = offset + stride;
        off_t length = lseek (params->fds[i], 0, SEEK_END);

        if (length >= 0)
            lseek (params->fds[i], 0, SEEK_SET);
        if ((linear || implicit) &&
            stride < (uint64_t)width * DMABUF_BYTES_PER_PIXEL)
            goto bounds;
        if (implicit && offset)
            goto failed;
        if (linear || implicit)
            end += stride * (height - 1);
        if (!stride || stride > INT_MAX || offset > INT_MAX || length < 0 ||
            end > (uint64_t)length)
            goto bounds;
    }
    if (dri3_pixmap_from_fds (&pixmap, dmabuf->screen, count, params->fds,
                              width, height, params->strides, params->offsets,
                              format->depth, DMABUF_BPP,
                              params->modifier) != Success ||
        !pixmap)
        goto failed;
    if (pixmap->drawable.width != width || pixmap->drawable.height != height ||
        pixmap->drawable.depth != format->depth ||
        pixmap->drawable.bitsPerPixel != DMABUF_BPP)
        goto failed;

    close_planes (params);
    buffer = calloc (1, sizeof (*buffer));
    if (!buffer)
        goto no_memory;
    buffer->base = (MbBuffer){
        .width = width,
        .height = height,
        .snapshot = dmabuf_snapshot,
        .destroy = dmabuf_buffer_destroy,
    };
    buffer->pixmap = pixmap;
    buffer->y_invert = flags & ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_Y_INVERT;
    if (!mb_buffer_publish (&buffer->base, client, id))
    {
        free (buffer);
        goto no_memory;
    }
    if (!id)
        zwp_linux_buffer_params_v1_send_created (resource,
                                                 buffer->base.resource);
    return;

bounds:
    error = ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_OUT_OF_BOUNDS;
    message = "DMA-BUF plane is out of bounds";
invalid:
    close_planes (params);
    wl_resource_post_error (resource, error, "%s", message);
    return;
failed:
    close_planes (params);
    if (pixmap)
        dmabuf->screen->DestroyPixmap (pixmap);
    if (id)
        wl_resource_post_error (
            resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_WL_BUFFER,
            "DRI3 could not import DMA-BUF");
    else
        zwp_linux_buffer_params_v1_send_failed (resource);
    return;
no_memory:
    dmabuf->screen->DestroyPixmap (pixmap);
    wl_resource_post_no_memory (resource);
}

static void
create_async (struct wl_client *client, struct wl_resource *resource,
              int32_t width, int32_t height, uint32_t format, uint32_t flags)
{
    create_buffer (client, resource, 0, width, height, format, flags);
}

static const struct zwp_linux_buffer_params_v1_interface
    params_implementation = {
        .destroy = destroy,
        .add = add_plane,
        .create = create_async,
        .create_immed = create_buffer,
    };

static void
create_params (struct wl_client *client, struct wl_resource *resource,
               uint32_t id)
{
    struct wl_resource *result;
    Params *params = calloc (1, sizeof (*params));

    if (!params)
    {
        wl_resource_post_no_memory (resource);
        return;
    }
    result = wl_resource_create (client, &zwp_linux_buffer_params_v1_interface,
                                 wl_resource_get_version (resource), id);
    if (!result)
    {
        free (params);
        wl_resource_post_no_memory (resource);
        return;
    }
    wl_resource_set_implementation (result, &params_implementation, params,
                                    params_destroyed);
}

static const struct zwp_linux_dmabuf_v1_interface implementation = {
    .destroy = destroy,
    .create_params = create_params,
};

static void
bind_dmabuf (struct wl_client *client, void *data, uint32_t version,
             uint32_t id)
{
    struct MbDmabuf *dmabuf = data;
    struct wl_resource *resource = wl_resource_create (
        client, &zwp_linux_dmabuf_v1_interface, version, id);

    if (!resource)
    {
        wl_client_post_no_memory (client);
        return;
    }
    wl_resource_set_implementation (resource, &implementation, NULL, NULL);
    for (size_t i = 0; i < ARRAY_SIZE (formats); i++)
    {
        Modifiers *modifiers = &dmabuf->modifiers[i];

        for (CARD32 m = 0; m < modifiers->count; m++)
        {
            CARD64 modifier = modifiers->values[m];

            if (version >= ZWP_LINUX_DMABUF_V1_MODIFIER_SINCE_VERSION)
                zwp_linux_dmabuf_v1_send_modifier (
                    resource, formats[i].fourcc,
                    modifier >> MODIFIER_HALF_BITS, (uint32_t)modifier);
            else if (modifier == DRM_FORMAT_MOD_INVALID)
            {
                zwp_linux_dmabuf_v1_send_format (resource, formats[i].fourcc);
                break;
            }
        }
    }
}
#endif /* DRI3 */

Bool
mb_dmabuf_initialize (void)
{
#ifdef DRI3
    ScreenPtr screen = screenInfo.screens[0];
    struct MbDmabuf *dmabuf;
    Bool supported = FALSE;

    if (!CheckExtension (DRI3_NAME))
        return TRUE;
    dmabuf = calloc (1, sizeof (*dmabuf));
    if (!dmabuf)
        return FALSE;
    compositor.dmabuf = dmabuf;
    dmabuf->screen = screen;
    for (size_t i = 0; i < ARRAY_SIZE (formats); i++)
    {
        CARD32 drawable_count;
        CARD64 *drawable_modifiers;
        Modifiers *modifiers = &dmabuf->modifiers[i];

        if (!PictureMatchFormat (screen, formats[i].depth,
                                 formats[i].render_format))
            continue;
        /* DRI3's modifier query indexes RGB FourCCs by its bpp argument:
         * 24 selects XRGB8888 and 32 selects ARGB8888. Both import at 32 bpp.
         */
        if (dri3_get_supported_modifiers (
                screen, &screen->root->drawable, formats[i].depth,
                formats[i].depth, &drawable_count, &drawable_modifiers,
                &modifiers->count, &modifiers->values) == Success)
        {
            free (drawable_modifiers);
            supported |= modifiers->count != 0;
        }
    }
    if (!supported)
    {
        mb_dmabuf_shutdown ();
        return TRUE;
    }
    dmabuf->global = wl_global_create (compositor.wl_display,
                                       &zwp_linux_dmabuf_v1_interface,
                                       DMABUF_VERSION, dmabuf, bind_dmabuf);
    if (!dmabuf->global)
        return FALSE;
#endif
    return TRUE;
}

void
mb_dmabuf_shutdown (void)
{
#ifdef DRI3
    struct MbDmabuf *dmabuf = compositor.dmabuf;

    if (!dmabuf)
        return;
    if (dmabuf->global)
        wl_global_destroy (dmabuf->global);
    for (size_t i = 0; i < ARRAY_SIZE (formats); i++)
        free (dmabuf->modifiers[i].values);
    free (dmabuf);
    compositor.dmabuf = NULL;
#endif
}
