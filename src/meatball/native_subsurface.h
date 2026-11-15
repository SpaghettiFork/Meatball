/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MEATBALL_NATIVE_SUBSURFACE_H
#define MEATBALL_NATIVE_SUBSURFACE_H

#include "native_surface.h"

typedef struct
{
    MbSurface *root, *surface;
    size_t index;
    int64_t x, y;
    Bool failed;
} MbSceneIterator;

extern Bool mb_subsurfaces_initialize (void);
extern void mb_subsurfaces_shutdown (void);
extern Bool mb_subsurface_capture (MbSurfaceCommit *commit);
extern void mb_subsurface_apply (MbSurfaceCommit *commit);
extern void mb_subsurface_commit_free (MbSurfaceCommit *commit);
extern void mb_subsurface_commit (MbSurfaceCommit *commit);
extern void mb_subsurface_surface_destroyed (MbSurface *surface);
extern MbSurface *mb_subsurface_root (MbSurface *surface);
extern MbSurface *mb_scene_next (MbSceneIterator *iterator);

#endif
