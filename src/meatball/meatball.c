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
#include "server.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xf86Module.h>

Compositor compositor;

void
meatball_version (unsigned long *version)
{
    *version =
        MODULE_VERSION_NUMERIC (MEATBALL_VERSION_MAJOR, MEATBALL_VERSION_MINOR,
                                MEATBALL_VERSION_PATCH);
}

_MEATBALL_BOOL
meatball_initialize (const struct meatball_config *config)
{
    struct stat directory;
    char *parent, *separator;

    if (compositor.wl_display)
        return MEATBALL_FALSE;
    if (!config || !config->socket_path || config->socket_path[0] != '/' ||
        config->socket_mode & ~MEATBALL_SOCKET_MODE_MASK)
    {
        mb_log (X_ERROR, "meatball: Invalid socket configuration\n");
        return MEATBALL_FALSE;
    }
    parent = strdup (config->socket_path);
    if (!parent)
        return MEATBALL_FALSE;
    separator = strrchr (parent, '/');
    if (separator == parent)
        separator[1] = '\0';
    else
        *separator = '\0';
    if (stat (parent, &directory) < 0 || !S_ISDIR (directory.st_mode))
    {
        mb_log (X_ERROR, "meatball: Invalid socket directory %s\n", parent);
        free (parent);
        return MEATBALL_FALSE;
    }
    free (parent);
    compositor.socket_path = strdup (config->socket_path);
    if (!compositor.socket_path)
        return MEATBALL_FALSE;
    compositor.wl_display = wl_display_create ();
    if (!compositor.wl_display)
        goto fail;
    compositor.wl_event_loop =
        wl_display_get_event_loop (compositor.wl_display);
    if (wl_display_add_socket (compositor.wl_display, compositor.socket_path) <
        0)
        goto fail;
    if (geteuid () == 0 &&
        chown (compositor.socket_path, directory.st_uid, directory.st_gid) < 0)
        goto fail;
    if (chmod (compositor.socket_path, config->socket_mode) < 0)
        goto fail;
    if (!mb_clients_initialize () || !mb_loop_initialize ())
        goto fail;
    mb_log (X_INFO, "meatball: Listening on %s\n", compositor.socket_path);
    return MEATBALL_TRUE;

fail:
    mb_log (X_ERROR, "meatball: Startup failed: %s\n", strerror (errno));
    meatball_shutdown ();
    return MEATBALL_FALSE;
}

void
meatball_shutdown (void)
{
    mb_loop_shutdown ();
    mb_clients_shutdown ();
    if (compositor.wl_display)
    {
        wl_display_destroy_clients (compositor.wl_display);
        wl_display_destroy (compositor.wl_display);
    }
    free (compositor.socket_path);
    memset (&compositor, 0, sizeof (compositor));
}
