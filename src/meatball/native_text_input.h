/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MEATBALL_NATIVE_TEXT_INPUT_H
#define MEATBALL_NATIVE_TEXT_INPUT_H

#include "native_surface.h"

typedef struct
{
    int fd;
    uint32_t size, depressed, latched, locked, group;
    int32_t rate, delay;
} MbTextKeyboard;

extern Bool mb_text_input_initialize (void);
extern void mb_text_input_shutdown (void);
extern void mb_text_input_focus (uint64_t seat, MbSurface *surface);
extern void mb_text_input_seat_removed (uint64_t seat);
extern Bool mb_text_input_popup_update (MbSurface *surface);
extern void mb_text_input_popup_destroyed (MbSurface *surface);
extern void mb_text_input_window_changed (MbSurface *surface);
extern Bool mb_text_input_key (uint64_t seat, uint32_t serial, uint32_t time,
                               uint32_t key, uint32_t state);
extern void mb_text_input_keyboard (uint64_t seat,
                                    const MbTextKeyboard *state);
extern void mb_text_input_modifiers (uint64_t seat, uint32_t serial,
                                     uint32_t depressed, uint32_t latched,
                                     uint32_t locked, uint32_t group);
extern void mb_text_input_repeat (uint64_t seat, int32_t rate, int32_t delay);

#endif
