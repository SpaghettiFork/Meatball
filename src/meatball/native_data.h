/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MEATBALL_NATIVE_DATA_H
#define MEATBALL_NATIVE_DATA_H

#include "native_surface.h"

struct MbData;
typedef struct
{
    MbSurface *surface;
    ScreenPtr screen;
    wl_fixed_t x, y;
    int32_t root_x, root_y;
    uint32_t time;
} MbDragPosition;

extern void mb_data_drag_motion (uint64_t seat,
                                 const MbDragPosition *position);
extern void mb_data_drag_drop (uint64_t seat);
extern void mb_data_drag_cancel (uint64_t seat);
extern Bool mb_data_icon_changed (MbSurface *surface);
extern void mb_data_surface_destroyed (MbSurface *surface);

extern Bool mb_data_initialize (void);
extern void mb_data_shutdown (void);
extern void mb_data_keyboard_focus (uint64_t seat, MbClient *client);
extern void mb_data_seat_removed (uint64_t seat);

#endif
