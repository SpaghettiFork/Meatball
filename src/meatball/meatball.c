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

#include "build_version.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xf86Module.h>

#define INVALID_SOCKET_FD (-1)
#define SOCKET_FD_PATH "/proc/self/fd/%d"

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
    struct stat directory, socket_info;
    char *parent, *separator;
    char *socket_fd_path = NULL;
    int socket_fd = INVALID_SOCKET_FD;

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
    socket_fd = open (compositor.socket_path, O_PATH | O_NOFOLLOW | O_CLOEXEC);
    if (socket_fd == INVALID_SOCKET_FD || fstat (socket_fd, &socket_info) < 0)
        goto fail;
    if (!S_ISSOCK (socket_info.st_mode))
    {
        errno = EINVAL;
        goto fail;
    }
    compositor.socket_uid =
        geteuid () == 0 ? directory.st_uid : socket_info.st_uid;
    if (geteuid () == 0 && fchownat (socket_fd, "", directory.st_uid,
                                     directory.st_gid, AT_EMPTY_PATH) < 0)
        goto fail;
    if (asprintf (&socket_fd_path, SOCKET_FD_PATH, socket_fd) < 0)
        goto fail;
    if (chmod (socket_fd_path, config->socket_mode) < 0)
        goto fail;
    if (!mb_windows_initialize () || !mb_clients_initialize () ||
        !mb_surfaces_initialize () || !mb_shm_initialize () ||
        !mb_single_pixel_initialize () || !mb_dmabuf_initialize () ||
        !mb_xdg_initialize () || !mb_viewport_initialize () ||
        !mb_subsurfaces_initialize () || !mb_input_initialize () ||
        !mb_text_input_initialize () || !mb_outputs_initialize () ||
        !mb_data_initialize () || !mb_selections_initialize () ||
        !mb_xdnd_initialize () || !mb_loop_initialize ())
        goto fail;
    close (socket_fd);
    free (socket_fd_path);
    mb_log (X_INFO, "meatball: Build %s\n", MEATBALL_BUILD_ID);
    mb_log (X_INFO, "meatball: Listening on %s\n", compositor.socket_path);
    return MEATBALL_TRUE;

fail:
    mb_log (X_ERROR, "meatball: Startup failed: %s\n", strerror (errno));
    if (socket_fd != INVALID_SOCKET_FD)
        close (socket_fd);
    free (socket_fd_path);
    meatball_shutdown ();
    return MEATBALL_FALSE;
}

_MEATBALL_BOOL
meatball_running (void) { return compositor.wl_display != NULL; }

void
meatball_shutdown (void)
{
    mb_loop_shutdown ();
    mb_xdnd_shutdown ();
    mb_selections_shutdown ();
    mb_data_shutdown ();
    mb_outputs_shutdown ();
    mb_text_input_shutdown ();
    mb_input_shutdown ();
    mb_subsurfaces_shutdown ();
    mb_viewport_shutdown ();
    mb_xdg_shutdown ();
    mb_dmabuf_shutdown ();
    mb_single_pixel_shutdown ();
    mb_shm_shutdown ();
    mb_surfaces_shutdown ();
    mb_clients_shutdown ();
    if (compositor.wl_display)
    {
        wl_display_destroy_clients (compositor.wl_display);
        wl_display_destroy (compositor.wl_display);
    }
    free (compositor.socket_path);
    memset (&compositor, 0, sizeof (compositor));
}
