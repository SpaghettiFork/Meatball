/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "server.h"

#include <picturestr.h>
#include <pixmapstr.h>
#include <wayland-server-protocol.h>

Bool
mb_surface_redraw (MbSurface *surface)
{
    WindowPtr window = mb_window_get (&surface->window, DixWriteAccess);
    PixmapPtr pixels;
    PicturePtr source = NULL, destination = NULL;
    PictFormatPtr source_format, destination_format;
    CARD32 format;
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

    if (!window || !window->realized || !surface->pixels)
        return TRUE;
    if (dixLookupResourceByType ((void **)&pixels, surface->pixels, RT_PIXMAP,
                                 surface->client->native,
                                 DixReadAccess) != Success)
        return FALSE;
    format = pixels->drawable.depth == MB_SHM_ARGB_DEPTH ? PICT_a8r8g8b8
                                                         : PICT_x8r8g8b8;
    source_format = PictureMatchFormat (surface->window.screen,
                                        pixels->drawable.depth, format);
    destination_format = PictureWindowFormat (window);
    if (!source_format || !destination_format)
        return FALSE;
    source = CreatePicture (FakeClientID (surface->client->native->index),
                            &pixels->drawable, source_format, 0, NULL,
                            surface->client->native, &error);
    if (!source)
        return FALSE;
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
    if (!pixman_transform_from_pixman_f_transform (&transform, &sampling) ||
        SetPictureTransform (source, &transform) != Success)
    {
        FreePicture (source, None);
        return FALSE;
    }
    destination = CreatePicture (FakeClientID (surface->client->native->index),
                                 &window->drawable, destination_format, 0,
                                 NULL, surface->client->native, &error);
    if (destination)
    {
        CompositePicture (PictOpSrc, source, NULL, destination, 0, 0, 0, 0, 0,
                          0, surface->width, surface->height);
        FreePicture (destination, None);
    }
    FreePicture (source, None);
    return destination != NULL;
}
