/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MEATBALL_NATIVE_CURSOR_H
#define MEATBALL_NATIVE_CURSOR_H

#include "native_surface.h"
#include <cursor.h>

#define MB_MAX_CURSOR_DIMENSION 512

extern CursorPtr mb_cursor_create (MbClient *client, MbSurface *surface,
                                   int64_t x, int64_t y);

#endif
