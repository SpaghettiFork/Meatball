/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MEATBALL_NATIVE_SURFACE_H
#define MEATBALL_NATIVE_SURFACE_H

#include "native_shm.h"
#include "native_window.h"
#include <pixman.h>

#define MB_COMPOSITOR_VERSION 1
#define MB_MAX_DIMENSION INT16_MAX

typedef struct MbSurface MbSurface;

struct MbSurface
{
    struct wl_list link;
    struct wl_resource *resource;
    MbClient *client;
    struct MbXdgSurface *xdg;
    MbWindow window;
    struct wl_list pending_frames;
    struct wl_list frames;
    pixman_region32_t pending_input;
    pixman_region32_t input;
    Bool pending_input_default;
    Bool input_default;
    Bool input_changed;
    Bool attach_pending;
    Bool damage_pending;
    MbShmBuffer *pending_buffer;
    MbShmBuffer *buffer;
    XID pixels;
    int32_t width, height;
    int32_t pending_x, pending_y;
    struct wl_event_source *frame_timer;
    Bool frame_timer_armed;
};

extern Bool mb_surface_redraw (MbSurface *surface);

extern Bool mb_surfaces_initialize (void);
extern void mb_surfaces_shutdown (void);

#endif
