/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MEATBALL_NATIVE_VIEWPORT_H
#define MEATBALL_NATIVE_VIEWPORT_H

#include "native_surface.h"

extern Bool mb_viewport_initialize (void);
extern void mb_viewport_shutdown (void);
extern void mb_viewport_surface_destroyed (MbSurface *surface);
extern Bool mb_viewport_size (MbSurface *surface, int32_t buffer_width,
                              int32_t buffer_height, int32_t *width,
                              int32_t *height);

#endif
