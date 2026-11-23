/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MEATBALL_NATIVE_INPUT_H
#define MEATBALL_NATIVE_INPUT_H

#include "native_data.h"
#include "native_surface.h"
#include "native_text_input.h"

struct MbInput;
extern Bool mb_input_begin_drag (uint64_t seat, MbSurface *origin,
                                 uint32_t serial, MbDragPosition *position);
extern uint64_t mb_input_begin_popup (struct wl_resource *seat,
                                      MbSurface *popup, MbSurface *parent,
                                      Bool nested, uint32_t serial);
extern void mb_input_popup_mapped (uint64_t seat, MbSurface *popup);
extern void mb_input_end_popup (uint64_t seat, MbSurface *popup,
                                MbSurface *parent);
extern uint64_t mb_input_drag_seat (ClientPtr client);
extern MbDragPosition mb_input_drag_position (MbSurface *root, int32_t x,
                                              int32_t y, uint32_t time);
extern void mb_input_end_drag (uint64_t seat);
extern uint64_t mb_input_seat_id (struct wl_resource *resource);
extern MbSurface *mb_input_keyboard_surface (uint64_t seat);
extern Bool mb_input_text_keyboard (uint64_t seat, MbTextKeyboard *state);
extern MbClient *mb_input_keyboard_client (uint64_t seat);
extern Bool mb_input_selection_serial (uint64_t seat, MbClient *client,
                                       uint32_t serial);
typedef struct
{
    DeviceIntPtr pointer;
    int32_t x, y;
    unsigned int button;
} MbInteractiveGrab;

extern Bool mb_input_take_grab (MbSurface *surface, struct wl_resource *seat,
                                uint32_t serial, MbInteractiveGrab *grab);
extern Bool mb_input_initialize (void);
extern void mb_input_shutdown (void);
extern Bool mb_input_dispatch (void);
extern Bool mb_input_select_window (MbWindow *window, MbClient *client);
extern Bool mb_input_observe_write (MbClient *client, const void *data,
                                    size_t size);
extern Bool mb_input_cursor_intersects (MbSurface *cursor, ScreenPtr screen,
                                        const pixman_box32_t *node,
                                        const pixman_box32_t *output);
extern Bool mb_input_cursor_visible (MbSurface *surface);
extern Bool mb_input_cursor_changed (MbSurface *surface);
extern Bool mb_input_capture_constraints (MbSurfaceCommit *commit);
extern void mb_input_apply_constraints (MbSurfaceCommit *commit);
extern void mb_input_free_constraints (MbSurfaceCommit *commit);
extern void mb_input_surface_destroyed (MbSurface *surface);

#endif
