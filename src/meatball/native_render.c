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
