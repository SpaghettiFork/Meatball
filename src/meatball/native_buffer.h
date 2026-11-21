/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MEATBALL_NATIVE_BUFFER_H
#define MEATBALL_NATIVE_BUFFER_H

#include "native_client.h"
#include <X11/extensions/renderproto.h>
#include <pixmap.h>

typedef struct MbBuffer MbBuffer;

struct MbBuffer
{
    struct wl_resource *resource;
    unsigned int refs, uses;
    int32_t width, height;
    CARD32 render_format;
    Bool y_invert;
    Bool solid;
    xRenderColor solid_color;
    PixmapPtr imported;
    void (*release_import) (MbBuffer *buffer);
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
extern Bool mb_single_pixel_initialize (void);
extern void mb_single_pixel_shutdown (void);
extern PixmapPtr mb_pixmap_upload (ScreenPtr screen, unsigned int width,
                                   unsigned int height, unsigned int depth,
                                   char *pixels);
/* Copy a newly imported SHM pixmap before releasing the client buffer. */
extern PixmapPtr mb_pixmap_snapshot (PixmapPtr imported);

#endif
