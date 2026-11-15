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
} Compositor;

extern Compositor compositor;

#endif
