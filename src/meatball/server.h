/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MEATBALL_SERVER_H
#define MEATBALL_SERVER_H

#include <xorg-server.h>

#include "meatball.h"
#include "meatball_log.h"
#include "native_client.h"
#include "native_cursor.h"
#include "native_data.h"
#include "native_dmabuf.h"
#include "native_input.h"
#include "native_keymap.h"
#include "native_output.h"
#include "native_selection.h"
#include "native_shm.h"
#include "native_subsurface.h"
#include "native_surface.h"
#include "native_viewport.h"
#include "native_window.h"
#include "native_xdg.h"
#include "native_xdnd.h"
#include <dixstruct.h>
#include <resource.h>
#include <wayland-server-core.h>

typedef struct
{
    struct wl_display *wl_display;
    struct wl_event_loop *wl_event_loop;
    char *socket_path;
    struct MbAtoms atoms;
    RESTYPE window_type;
    struct wl_global *xdg_global;
    struct wl_global *shm_global;
    struct wl_global *surface_global;
    struct wl_global *tearing_control_global;
    struct wl_global *viewport_global;
    struct wl_global *subsurface_global;
    struct wl_list surfaces;
    struct wl_list clients;
    struct MbInput *input;
    struct MbOutputs *outputs;
    struct MbData *data;
    struct MbDmabuf *dmabuf;
    struct MbSelections *selections;
    struct MbXdnd *xdnd;
    struct wl_listener client_created;
    MbClient *bridge_client;
    int clients_fd;
    Bool clients_initialized;
    Bool clients_stopping;
    Bool loop_notify_registered;
    Bool loop_block_registered;
    Bool xdg_shapes_dirty;
} Compositor;

extern Compositor compositor;

extern Bool mb_loop_initialize (void);
extern void mb_loop_shutdown (void);

#endif
