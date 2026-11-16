/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MEATBALL_NATIVE_XDG_H
#define MEATBALL_NATIVE_XDG_H

#include "native_surface.h"

struct MbXdgSurface;
extern Bool mb_xdg_initialize (void);
extern void mb_xdg_shutdown (void);
extern void mb_xdg_refresh_shapes (void);
extern Bool mb_xdg_constrain_scene (MbSurface *surface, pixman_box32_t *bounds,
                                    int32_t *anchor_x, int32_t *anchor_y);
extern Bool mb_xdg_commit (MbSurface *surface);
extern void mb_xdg_capture (MbSurfaceCommit *commit);
extern void mb_xdg_apply (MbSurfaceCommit *commit);
extern void mb_xdg_commit_free (MbSurfaceCommit *commit);
extern Bool mb_xdg_map (MbSurface *surface);
extern Bool mb_xdg_surface_destroy_request (MbSurface *surface);
extern void mb_xdg_surface_destroyed (MbSurface *surface);
extern void mb_xdg_event (MbSurface *surface, const xEvent *event);

#endif
