/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MEATBALL_NATIVE_SHM_H
#define MEATBALL_NATIVE_SHM_H

#include "native_client.h"
#include <pixmap.h>

#define MB_SHM_BYTES_PER_PIXEL 4
#define MB_SHM_BITS_PER_PIXEL 32
#define MB_SHM_ARGB_DEPTH 32
#define MB_SHM_XRGB_DEPTH 24

extern Bool mb_shm_initialize (void);
extern void mb_shm_shutdown (void);

#endif
