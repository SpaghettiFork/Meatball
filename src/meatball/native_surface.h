/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MEATBALL_NATIVE_SURFACE_H
#define MEATBALL_NATIVE_SURFACE_H

#include "native_buffer.h"
#include "native_shm.h"
#include "native_window.h"
#include <picture.h>
#include <pixman.h>

#define MB_COMPOSITOR_VERSION 4
#define MB_MAX_DIMENSION INT16_MAX

typedef struct MbSurface MbSurface;
struct MbSubsurface;

typedef enum
{
    MB_ROLE_NONE,
    MB_ROLE_XDG,
    MB_ROLE_SUBSURFACE,
    MB_ROLE_CURSOR,
    MB_ROLE_DRAG_ICON,
    MB_ROLE_INPUT_POPUP,
} MbSurfaceRole;

typedef struct
{
    struct MbSubsurface *subsurface;
    int32_t x, y;
} MbPlacement;

typedef struct
{
    Bool source_set, destination_set;
    wl_fixed_t x, y, width, height;
    int32_t destination_width, destination_height;
} MbViewportState;

typedef struct
{
    unsigned int refs;
    MbBuffer *buffer;
    XID pixels;
} MbSurfaceContent;

typedef struct
{
    struct MbXdgSurface *owner;
    Bool geometry_set;
    int32_t x, y, width, height;
    int32_t popup_x, popup_y;
    uint32_t serial;
} MbXdgCommitState;

typedef struct MbSurfaceCommit
{
    struct wl_list link;
    struct wl_list dependency_link, dependencies;
    struct MbSurfaceCommit *parent, *resume;
    MbSurface *surface;
    MbSurfaceContent *content;
    MbXdgCommitState xdg;
    struct wl_list frames, constraints;
    pixman_region32_t input;
    Bool input_default;
    MbViewportState crop;
    int32_t width, height, scale, transform;
    int32_t offset_x, offset_y;
    Bool synchronized;
    Bool tearing_async;
    MbPlacement *stack;
    size_t stack_count;
} MbSurfaceCommit;

struct MbSurface
{
    struct wl_list link;
    struct wl_resource *resource;
    MbClient *client;
    MbSurfaceRole role;
    unsigned int commit_count;
    struct MbSubsurface *subsurface;
    struct wl_list pending_stack, pending_self;
    MbPlacement *stack;
    size_t stack_count;
    struct MbXdgSurface *xdg;
    struct MbViewport *viewport;
    struct wl_resource *tearing_control;
    struct wl_resource *input_popup;
    Bool tearing_async, pending_tearing_async;
    MbViewportState crop, pending_crop;
    MbWindow window;
    struct wl_list pending_frames, constraints;
    struct wl_list frames;
    pixman_region32_t pending_input;
    pixman_region32_t input;
    Bool pending_input_default;
    Bool input_default;
    Bool input_changed;
    Bool attach_pending;
    MbBuffer *pending_buffer;
    MbSurfaceContent *content;
    struct wl_list commits;
    int32_t width, height;
    int32_t scale, pending_scale;
    int32_t transform, pending_transform;
    int32_t pending_x, pending_y;
    int32_t offset_x, offset_y;
    int32_t scene_width, scene_height;
    int32_t scene_x, scene_y;
    int64_t native_dx, native_dy;
    struct wl_event_source *frame_timer;
    XID present_event;
    CARD32 frame_serial, present_serial;
    CARD8 present_kind;
    int present_opcode;
    Bool present_redraw, present_disabled;
};

extern PicturePtr mb_scene_picture (MbSurface *surface);
extern Bool mb_surface_redraw (MbSurface *surface, WindowPtr window,
                               CARD32 serial, Bool *submitted);
extern Bool mb_surface_update (MbSurface *surface);
extern void mb_surface_commit_apply (MbSurfaceCommit *commit);
extern void mb_surface_commit_free (MbSurfaceCommit *commit);

extern Bool mb_surfaces_initialize (void);
extern void mb_surfaces_shutdown (void);

#endif
