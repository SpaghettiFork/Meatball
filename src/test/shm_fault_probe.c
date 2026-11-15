/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "probe.h"

#include <assert.h>
#include <shmint.h>
#include <unistd.h>

extern PixmapPtr __real_shm_create_pixmap_from_fd (ScreenPtr screen, int fd,
                                                   CARD16 width, CARD16 height,
                                                   CARD8 depth, CARD8 bpp,
                                                   CARD32 stride,
                                                   CARD32 offset);
extern PixmapPtr __wrap_shm_create_pixmap_from_fd (ScreenPtr screen, int fd,
                                                   CARD16 width, CARD16 height,
                                                   CARD8 depth, CARD8 bpp,
                                                   CARD32 stride,
                                                   CARD32 offset);

PixmapPtr
__wrap_shm_create_pixmap_from_fd (ScreenPtr screen, int fd, CARD16 width,
                                  CARD16 height, CARD8 depth, CARD8 bpp,
                                  CARD32 stride, CARD32 offset)
{
    PixmapPtr pixmap = __real_shm_create_pixmap_from_fd (
        screen, fd, width, height, depth, bpp, stride, offset);

    assert (pixmap);
    assert (ftruncate (fd, 0) == 0);
    return pixmap;
}

void
mb_probe_start (void)
{
}

void
mb_probe_stop (void)
{
}
