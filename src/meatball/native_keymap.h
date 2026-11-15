/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MEATBALL_NATIVE_KEYMAP_H
#define MEATBALL_NATIVE_KEYMAP_H

#include "native_client.h"
#include <input.h>

/* Returns an owned, sealed descriptor containing a NUL-terminated keymap. */
extern int mb_keymap_create (DeviceIntPtr keyboard, uint32_t *size);

#endif
