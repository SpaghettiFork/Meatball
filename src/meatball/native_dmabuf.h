/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MEATBALL_NATIVE_DMABUF_H
#define MEATBALL_NATIVE_DMABUF_H

#include "native_buffer.h"

struct MbDmabuf;
extern Bool mb_dmabuf_initialize (void);
extern void mb_dmabuf_shutdown (void);
extern Bool mb_dmabuf_dri3_reply (MbClient *client, const void *message,
                                  size_t size);

#endif
