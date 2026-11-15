/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MEATBALL_NATIVE_SHM_H
#define MEATBALL_NATIVE_SHM_H

#include "native_client.h"
#include <pixmap.h>

#define MB_SHM_BYTES_PER_PIXEL 4
#define MB_SHM_BITS_PER_PIXEL 32
#define MB_SHM_ARGB_DEPTH 32
#define MB_SHM_XRGB_DEPTH 24
#define MB_SHM_MAX_SNAPSHOT_BYTES (256 * 1024 * 1024)

struct MbShmPool;
typedef struct
{
    struct wl_resource *resource;
    struct MbShmPool *pool;
    unsigned int refs;
    int32_t width, height, stride, offset;
    uint32_t format;
} MbShmBuffer;

extern Bool mb_shm_initialize (void);
extern void mb_shm_shutdown (void);
/* Returns an owned reference for a buffer implemented by this renderer. */
extern MbShmBuffer *mb_shm_buffer_acquire (struct wl_resource *resource);
extern void mb_shm_buffer_unref (MbShmBuffer *buffer);
/* Returns an owned pixmap containing an independent copy of the SHM pixels. */
extern PixmapPtr mb_shm_snapshot (MbShmBuffer *buffer, ScreenPtr screen);

#endif
