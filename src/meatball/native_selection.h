/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MEATBALL_NATIVE_SELECTION_H
#define MEATBALL_NATIVE_SELECTION_H

#include "native_data.h"

struct MbSelections;

typedef void (*MbSelectionNotify) (void *data, MbSourceEvent event,
                                   const char *mime, uint32_t action);
extern MbDataSource *mb_selection_import (Window owner, Atom selection,
                                          Time time, const CARD32 *targets,
                                          size_t count, uint32_t actions,
                                          MbSelectionNotify notify,
                                          void *data);
extern void mb_selection_time (MbDataSource *source, Time time);
extern Bool mb_selection_claim_drag (MbDataSource *source);
extern Window mb_selection_window (void);
extern Bool mb_selection_watch (Window window, Bool watch);

extern Bool mb_selections_initialize (void);
extern void mb_selections_shutdown (void);
extern void mb_selections_dispatch (void);

#endif
