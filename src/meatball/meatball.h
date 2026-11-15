/**
 * Spaghetti Display Server
 * Copyright (C) 2025  SpaghettiFork
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef _MEATBALL_H
#define _MEATBALL_H

#include <X11/Xfuncproto.h>

#define MEATBALL_VERSION_MAJOR 2
#define MEATBALL_VERSION_MINOR 0
#define MEATBALL_VERSION_PATCH 0
#define MEATBALL_EXTENSION_NAME "MEATBALL"
#define MEATBALL_SOCKET_PREFIX "wayland-meatball-"
#define MEATBALL_DEFAULT_SOCKET_MODE 0600
#define MEATBALL_SOCKET_MODE_MASK 0777

typedef int _MEATBALL_BOOL;

enum MEATBALL_BOOL
{
    MEATBALL_FALSE = 0,
    MEATBALL_TRUE = 1
};

struct meatball_config
{
    /* Absolute Unix socket path. Copied during initialization. */
    const char *socket_path;
    unsigned int socket_mode;
};

extern _X_EXPORT void meatball_version (unsigned long *version);

/* Configuration storage remains owned by the caller. Failure releases all
 * state allocated by this call. A running instance rejects initialization.
 */
extern _X_EXPORT _MEATBALL_BOOL
meatball_initialize (const struct meatball_config *config);

/* Safe before initialization and after a previous shutdown. */
extern _X_EXPORT void meatball_shutdown (void);

#endif
