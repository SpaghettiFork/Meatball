/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "server.h"

#include <picturestr.h>
#include <pixmapstr.h>
#include <present.h>
#include <wayland-server-protocol.h>

static PicturePtr
surface_picture (MbSurface *surface)
{
    MbBuffer *buffer = surface->content->buffer;
    PixmapPtr pixels = buffer->imported;
    PicturePtr source;
    PictFormatPtr source_format;
    pixman_f_transform_t sampling = {
        .m = { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 1 } }
    };
    PictTransform transform;
    double x = 0, y = 0, width, height, scale_x, scale_y;
    Bool rotated = surface->transform == WL_OUTPUT_TRANSFORM_90 ||
                   surface->transform == WL_OUTPUT_TRANSFORM_270 ||
                   surface->transform == WL_OUTPUT_TRANSFORM_FLIPPED_90 ||
                   surface->transform == WL_OUTPUT_TRANSFORM_FLIPPED_270;
    int error;

    if (buffer->solid)
        return CreateSolidPicture (None, &buffer->solid_color, &error);
    if (!pixels && dixLookupResourceByType (
                       (void **)&pixels, surface->content->pixels, RT_PIXMAP,
                       surface->client->native, DixReadAccess) != Success)
        return NULL;
    source_format = PictureMatchFormat (
        surface->window.screen, pixels->drawable.depth, buffer->render_format);
    if (!source_format)
        return NULL;
    source = CreatePicture (FakeClientID (surface->client->native->index),
                            &pixels->drawable, source_format, 0, NULL,
                            surface->client->native, &error);
    if (!source)
        return NULL;
    width = pixels->drawable.width;
    height = pixels->drawable.height;
    if (surface->crop.source_set)
    {
        x = wl_fixed_to_double (surface->crop.x) * surface->scale;
        y = wl_fixed_to_double (surface->crop.y) * surface->scale;
        scale_x = wl_fixed_to_double (surface->crop.width) * surface->scale /
                  surface->width;
        scale_y = wl_fixed_to_double (surface->crop.height) * surface->scale /
                  surface->height;
    }
    else
    {
        scale_x = (rotated ? height : width) / surface->width;
        scale_y = (rotated ? width : height) / surface->height;
    }
    /* Render maps destination coordinates back into the buffer. */
    switch (surface->transform)
    {
    case WL_OUTPUT_TRANSFORM_NORMAL:
        sampling.m[0][0] = scale_x;
        sampling.m[0][2] = x;
        sampling.m[1][1] = scale_y;
        sampling.m[1][2] = y;
        break;
    case WL_OUTPUT_TRANSFORM_90:
        sampling.m[0][1] = scale_y;
        sampling.m[0][2] = y;
        sampling.m[1][0] = -scale_x;
        sampling.m[1][2] = height - x;
        break;
    case WL_OUTPUT_TRANSFORM_180:
        sampling.m[0][0] = -scale_x;
        sampling.m[0][2] = width - x;
        sampling.m[1][1] = -scale_y;
        sampling.m[1][2] = height - y;
        break;
    case WL_OUTPUT_TRANSFORM_270:
        sampling.m[0][1] = -scale_y;
        sampling.m[0][2] = width - y;
        sampling.m[1][0] = scale_x;
        sampling.m[1][2] = x;
        break;
    case WL_OUTPUT_TRANSFORM_FLIPPED:
        sampling.m[0][0] = -scale_x;
        sampling.m[0][2] = width - x;
        sampling.m[1][1] = scale_y;
        sampling.m[1][2] = y;
        break;
    case WL_OUTPUT_TRANSFORM_FLIPPED_90:
        sampling.m[0][1] = scale_y;
        sampling.m[0][2] = y;
        sampling.m[1][0] = scale_x;
        sampling.m[1][2] = x;
        break;
    case WL_OUTPUT_TRANSFORM_FLIPPED_180:
        sampling.m[0][0] = scale_x;
        sampling.m[0][2] = x;
        sampling.m[1][1] = -scale_y;
        sampling.m[1][2] = height - y;
        break;
    case WL_OUTPUT_TRANSFORM_FLIPPED_270:
        sampling.m[0][1] = -scale_y;
        sampling.m[0][2] = width - y;
        sampling.m[1][0] = -scale_x;
        sampling.m[1][2] = height - x;
        break;
    }
    if (buffer->y_invert)
    {
        sampling.m[1][0] = -sampling.m[1][0];
        sampling.m[1][1] = -sampling.m[1][1];
        sampling.m[1][2] = height - sampling.m[1][2];
    }
    if (!pixman_transform_from_pixman_f_transform (&transform, &sampling) ||
        SetPictureTransform (source, &transform) != Success)
    {
        FreePicture (source, None);
        return NULL;
    }
    return source;
}

static PicturePtr
scene_picture (MbSurface *surface, PictFormatPtr format)
{
    PicturePtr canvas = NULL;
    PixmapPtr pixels;
    MbSceneIterator iterator = { .root = surface, .surface = surface };
    MbSurface *node;
    xRenderColor clear = { 0 };
    xRectangle rectangle = { 0, 0, surface->scene_width,
                             surface->scene_height };
    int error;

    if (!format || !surface->content)
        return NULL;
    pixels = surface->window.screen->CreatePixmap (
        surface->window.screen, surface->scene_width, surface->scene_height,
        format->depth, 0);
    if (!pixels)
        return NULL;
    canvas = CreatePicture (FakeClientID (surface->client->native->index),
                            &pixels->drawable, format, 0, NULL,
                            surface->client->native, &error);
    surface->window.screen->DestroyPixmap (pixels);
    if (!canvas)
        return NULL;
    CompositeRects (PictOpSrc, canvas, &clear, 1, &rectangle);
    while ((node = mb_scene_next (&iterator)))
    {
        PicturePtr source = surface_picture (node);

        if (!source)
            goto fail;
        CompositePicture (PictOpOver, source, NULL, canvas, 0, 0, 0, 0,
                          iterator.x - surface->scene_x,
                          iterator.y - surface->scene_y, node->width,
                          node->height);
        FreePicture (source, None);
    }
    if (!iterator.failed)
        return canvas;

fail:
    FreePicture (canvas, None);
    return NULL;
}

PicturePtr
mb_scene_picture (MbSurface *surface)
{
    PictFormatPtr format = PictureMatchFormat (
        surface->window.screen, MB_SHM_ARGB_DEPTH, PICT_a8r8g8b8);

    return scene_picture (surface, format);
}

Bool
mb_surface_redraw (MbSurface *surface, WindowPtr window, CARD32 serial,
                   Bool *submitted)
{
    PicturePtr destination = NULL, canvas;
    PictFormatPtr format;
    int error;

    *submitted = FALSE;
    if (!window->realized || !surface->content)
        return TRUE;
    format = PictureWindowFormat (window);
    if (serial && format && window->drawable.width == surface->scene_width &&
        window->drawable.height == surface->scene_height)
    {
        canvas = scene_picture (surface, format);
        if (canvas)
        {
            uint32_t options = PresentOptionCopy;

            if (surface->tearing_async)
                options |= PresentOptionAsyncMayTear;
            /* Present keeps its own reference for the lifetime of the copy. */
            error = present_pixmap (window, (PixmapPtr)canvas->pDrawable,
                                    serial, NULL, NULL, 0, 0, NULL, NULL, NULL,
#ifdef DRI3
                                    NULL, NULL, 0, 0,
#endif
                                    options, 0, 0, 0, NULL, 0);
            FreePicture (canvas, None);
            if (error == Success)
            {
                *submitted = TRUE;
                return TRUE;
            }
        }
    }

    canvas = mb_scene_picture (surface);
    if (!canvas)
        return FALSE;
    if (format)
        destination = CreatePicture (
            FakeClientID (surface->client->native->index), &window->drawable,
            format, 0, NULL, surface->client->native, &error);
    if (destination)
    {
        CompositePicture (PictOpSrc, canvas, NULL, destination, 0, 0, 0, 0, 0,
                          0, surface->scene_width, surface->scene_height);
        FreePicture (destination, None);
    }
    FreePicture (canvas, None);
    return destination != NULL;
}
