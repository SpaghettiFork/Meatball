/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MEATBALL_NATIVE_WINDOW_H
#define MEATBALL_NATIVE_WINDOW_H

#include "native_client.h"
#include <windowstr.h>

#define MB_INITIAL_WINDOW_SIZE 1
#define MB_PROPERTY_FORMAT_8 8
#define MB_PROPERTY_FORMAT_32 32

struct MbAtoms
{
    Atom wm_protocols;
    Atom wm_delete_window;
    Atom net_wm_name;
    Atom utf8_string;
    Atom net_wm_pid;
    Atom net_wm_state;
    Atom net_wm_fullscreen;
    Atom net_wm_maximized_horz;
    Atom net_wm_maximized_vert;
    Atom net_wm_focused;
    Atom wm_change_state;
    Atom gtk_frame_extents;
};

typedef struct
{
    XID id;
    XID colormap;
    uint64_t owner;
    ScreenPtr screen;
    Bool destroying;
} MbWindow;

extern Bool mb_windows_initialize (void);
extern Bool mb_window_create (MbWindow *window, MbClient *owner,
                              ScreenPtr screen);
extern WindowPtr mb_window_get (MbWindow *window, Mask access);
extern void mb_window_destroy (MbWindow *window);

#endif
