/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MEATBALL_NATIVE_XDND_H
#define MEATBALL_NATIVE_XDND_H

#include "native_data.h"

struct MbXdnd;
extern Bool mb_xdnd_initialize (void);
extern void mb_xdnd_shutdown (void);
extern void mb_xdnd_dispatch (void);
extern void mb_xdnd_surface_destroyed (MbSurface *surface);
extern Bool mb_xdnd_advertise (MbSurface *surface);
extern Bool mb_xdnd_event (MbSurface *surface, const xEvent *event);
extern Bool mb_xdnd_motion (uint64_t seat, MbDataSource *source,
                            const MbDragPosition *position);
extern Bool mb_xdnd_drop (uint64_t seat);
extern void mb_xdnd_cancel (uint64_t seat);

#endif
