/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MEATBALL_NATIVE_SELECTION_H
#define MEATBALL_NATIVE_SELECTION_H

#include "native_data.h"

struct MbSelections;

extern Bool mb_selections_initialize (void);
extern void mb_selections_shutdown (void);
extern void mb_selections_dispatch (void);

#endif
