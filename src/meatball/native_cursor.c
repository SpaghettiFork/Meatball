/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "server.h"

#include <cursorstr.h>
#include <picturestr.h>
#include <servermd.h>
#include <stdlib.h>

#define CURSOR_ALPHA_SHIFT 24
#define CURSOR_GREEN_SHIFT 8
#define CURSOR_RED_SHIFT 16
#define CURSOR_CHANNEL_MASK 0xff
#define CURSOR_OPAQUE_ALPHA 0x80
#define CURSOR_COLOR_CHANNELS 3
#define BITS_PER_BYTE 8

CursorPtr
mb_cursor_create (MbClient *client, MbSurface *surface, int64_t x, int64_t y)
{
    PicturePtr picture = NULL;
    CursorMetricRec metrics = { .width = 1, .height = 1 };
    CursorPtr cursor;
    unsigned char *source = NULL, *mask = NULL;
    CARD32 *argb = NULL;
    int64_t left = 0, top = 0, width = 1, height = 1;
    XID id = FakeClientID (client->native->index);
    ScreenPtr screen;
    int row, column, stride;

    if (surface && surface->pixels)
    {
        x -= surface->scene_x;
        y -= surface->scene_y;
        left = min (0, x);
        top = min (0, y);
        width = max (surface->scene_width, x + 1) - left;
        height = max (surface->scene_height, y + 1) - top;
        if (width > MB_MAX_CURSOR_DIMENSION ||
            height > MB_MAX_CURSOR_DIMENSION)
        {
            wl_client_post_implementation_error (
                client->wayland, "cursor exceeds %d pixels per direction",
                MB_MAX_CURSOR_DIMENSION);
            return NULL;
        }
        metrics = (CursorMetricRec){ width, height, x - left, y - top };
        picture = mb_scene_picture (surface);
        if (!picture)
            return NULL;
    }
    argb = calloc (width * height, sizeof (*argb));
    source = calloc (BitmapBytePad (width), height);
    mask = calloc (BitmapBytePad (width), height);
    if (!argb || !source || !mask)
        goto fail;
    if (picture)
    {
        screen = surface->window.screen;
        for (row = 0; row < surface->scene_height; row++)
            screen->GetImage (picture->pDrawable, 0, row, surface->scene_width,
                              1, ZPixmap, ~0UL,
                              (char *)(argb + (row - top) * width - left));
        FreePicture (picture, None);
    }
    /* Supply monochrome planes for screens without an ARGB cursor path. */
    stride = BitmapBytePad (width);
    for (row = 0; row < height; row++)
        for (column = 0; column < width; column++)
        {
            CARD32 pixel = argb[row * width + column];
            unsigned int alpha = pixel >> CURSOR_ALPHA_SHIFT;
            unsigned int intensity =
                ((pixel >> CURSOR_RED_SHIFT) & CURSOR_CHANNEL_MASK) +
                ((pixel >> CURSOR_GREEN_SHIFT) & CURSOR_CHANNEL_MASK) +
                (pixel & CURSOR_CHANNEL_MASK);
            unsigned int bit = column % BITS_PER_BYTE;
            size_t byte = row * stride + column / BITS_PER_BYTE;

            if (BITMAP_BIT_ORDER == MSBFirst)
                bit = BITS_PER_BYTE - 1 - bit;
            if (alpha >= CURSOR_OPAQUE_ALPHA)
            {
                mask[byte] |= 1u << bit;
                if (intensity <= alpha * CURSOR_COLOR_CHANNELS / 2)
                    source[byte] |= 1u << bit;
            }
        }
    /* AllocARGBCursor takes ownership of all three pixel allocations. */
    if (AllocARGBCursor (source, mask, argb, &metrics, 0, 0, 0, UINT16_MAX,
                         UINT16_MAX, UINT16_MAX, &cursor, client->native,
                         id) != Success)
        return NULL;
    if (!AddResource (id, RT_CURSOR, cursor))
        return NULL;
    return cursor;

fail:
    if (picture)
        FreePicture (picture, None);
    free (argb);
    free (source);
    free (mask);
    return NULL;
}
