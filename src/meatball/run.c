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
#include <string.h>

static void
loop_wakeup (void *data, int result)
{
}

static void
loop_block (void *data, void *timeout)
{
    mb_clients_check ();
    mb_xdg_refresh_shapes ();
    wl_event_loop_dispatch_idle (compositor.wl_event_loop);
    if (mb_input_dispatch ())
        AdjustWaitForDelay (timeout, 0);
    wl_display_flush_clients (compositor.wl_display);
}

static void
loop_ready (int fd, int ready, void *data)
{
    if ((ready & X_NOTIFY_ERROR) ||
        wl_event_loop_dispatch (compositor.wl_event_loop, 0) < 0)
    {
        mb_log (X_ERROR, "meatball: Wayland dispatch failed: %s\n",
                ready & X_NOTIFY_ERROR ? "event loop fd error"
                                       : strerror (errno));
        meatball_shutdown ();
    }
}

Bool
mb_loop_initialize (void)
{
    int fd = wl_event_loop_get_fd (compositor.wl_event_loop);

    if (!SetNotifyFd (fd, loop_ready, X_NOTIFY_READ, NULL))
        return FALSE;
    compositor.loop_notify_registered = TRUE;
    if (!RegisterBlockAndWakeupHandlers (loop_block, loop_wakeup, NULL))
    {
        mb_loop_shutdown ();
        return FALSE;
    }
    compositor.loop_block_registered = TRUE;
    return TRUE;
}

void
mb_loop_shutdown (void)
{
    if (compositor.loop_notify_registered)
    {
        RemoveNotifyFd (wl_event_loop_get_fd (compositor.wl_event_loop));
        compositor.loop_notify_registered = FALSE;
    }
    if (compositor.loop_block_registered)
    {
        RemoveBlockAndWakeupHandlers (loop_block, loop_wakeup, NULL);
        compositor.loop_block_registered = FALSE;
    }
}
