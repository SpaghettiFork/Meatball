/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MEATBALL_NATIVE_INPUT_H
#define MEATBALL_NATIVE_INPUT_H

#include "native_surface.h"

struct MbInput;
extern Bool mb_input_initialize (void);
extern void mb_input_shutdown (void);
extern Bool mb_input_dispatch (void);
extern Bool mb_input_select_window (MbWindow *window, MbClient *client);
extern Bool mb_input_observe_write (MbClient *client, const void *data,
                                    size_t size);
extern void mb_input_surface_destroyed (MbSurface *surface);

#endif
