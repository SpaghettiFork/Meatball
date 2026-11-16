/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MEATBALL_NATIVE_DATA_H
#define MEATBALL_NATIVE_DATA_H

#include "native_surface.h"

struct MbData;

extern Bool mb_data_initialize (void);
extern void mb_data_shutdown (void);
extern void mb_data_keyboard_focus (uint64_t seat, MbClient *client);
extern void mb_data_seat_removed (uint64_t seat);

#endif
