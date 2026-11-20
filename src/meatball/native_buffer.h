/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MEATBALL_NATIVE_BUFFER_H
#define MEATBALL_NATIVE_BUFFER_H

#include "native_client.h"
#include <pixmap.h>

typedef struct MbBuffer MbBuffer;

struct MbBuffer
{
    struct wl_resource *resource;
    unsigned int refs;
    int32_t width, height;
    CARD32 render_format;
    PixmapPtr (*snapshot) (MbBuffer *buffer, ScreenPtr screen);
    void (*destroy) (MbBuffer *buffer);
};

#define MB_RGB565_DEPTH 16
#define MB_XRGB1555_DEPTH 15
#define MB_RGB16_BPP 16
#define MB_MAX_SNAPSHOT_BYTES (256 * 1024 * 1024)

extern Bool mb_buffer_publish (MbBuffer *buffer, struct wl_client *client,
                               uint32_t id);
extern MbBuffer *mb_buffer_acquire (struct wl_resource *resource);
extern void mb_buffer_unref (MbBuffer *buffer);
extern PixmapPtr mb_pixmap_upload (ScreenPtr screen, unsigned int width,
                                   unsigned int height, unsigned int depth,
                                   char *pixels);
/* Read the client pixels before release, into an independent pixmap. */
extern PixmapPtr mb_pixmap_snapshot (PixmapPtr imported, Bool y_invert);

#endif
