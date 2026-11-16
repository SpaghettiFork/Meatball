/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MEATBALL_NATIVE_OUTPUT_H
#define MEATBALL_NATIVE_OUTPUT_H

#include "native_surface.h"

struct MbOutputs;

extern Bool mb_outputs_initialize (void);
extern void mb_outputs_shutdown (void);
extern void mb_outputs_refresh (void);
extern void mb_outputs_surface_destroyed (MbSurface *surface);

#endif
