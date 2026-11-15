/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MEATBALL_NATIVE_SURFACE_H
#define MEATBALL_NATIVE_SURFACE_H

#include "native_shm.h"
#include "native_window.h"
#include <pixman.h>

#define MB_COMPOSITOR_VERSION 4
#define MB_MAX_DIMENSION INT16_MAX

typedef struct MbSurface MbSurface;

typedef struct
{
    Bool source_set, destination_set;
    wl_fixed_t x, y, width, height;
    int32_t destination_width, destination_height;
} MbViewportState;

typedef struct
{
    unsigned int refs;
    MbShmBuffer *buffer;
    XID pixels;
} MbSurfaceContent;

typedef struct MbSurfaceCommit
{
    struct wl_list link;
    MbSurface *surface;
    MbSurfaceContent *content;
    struct wl_list frames;
    pixman_region32_t input;
    Bool input_default;
    MbViewportState crop;
    int32_t width, height, scale, transform;
    int32_t offset_x, offset_y;
} MbSurfaceCommit;

struct MbSurface
{
    struct wl_list link;
    struct wl_resource *resource;
    MbClient *client;
    struct MbXdgSurface *xdg;
    struct MbViewport *viewport;
    MbViewportState crop, pending_crop;
    MbWindow window;
    struct wl_list pending_frames;
    struct wl_list frames;
    pixman_region32_t pending_input;
    pixman_region32_t input;
    Bool pending_input_default;
    Bool input_default;
    Bool input_changed;
    Bool attach_pending;
    MbShmBuffer *pending_buffer;
    MbShmBuffer *buffer;
    XID pixels;
    MbSurfaceContent *content;
    struct wl_list commits;
    int32_t width, height;
    int32_t scale, pending_scale;
    int32_t transform, pending_transform;
    int32_t pending_x, pending_y;
    int32_t offset_x, offset_y;
    int32_t scene_width, scene_height;
    struct wl_event_source *frame_timer;
    Bool frame_timer_armed;
};

extern Bool mb_surface_redraw (MbSurface *surface);
extern Bool mb_surface_update (MbSurface *surface);
extern void mb_surface_commit_apply (MbSurfaceCommit *commit);
extern void mb_surface_commit_free (MbSurfaceCommit *commit);

extern Bool mb_surfaces_initialize (void);
extern void mb_surfaces_shutdown (void);

#endif
