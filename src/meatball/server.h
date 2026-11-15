/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MEATBALL_SERVER_H
#define MEATBALL_SERVER_H

#include <xorg-server.h>

#include "meatball.h"
#include "meatball_log.h"
#include <dixstruct.h>
#include <wayland-server-core.h>

typedef struct
{
    struct wl_display *wl_display;
    struct wl_event_loop *wl_event_loop;
    char *socket_path;
    Bool loop_notify_registered;
    Bool loop_block_registered;
} Compositor;

extern Compositor compositor;

extern Bool mb_loop_initialize (void);
extern void mb_loop_shutdown (void);

#endif
