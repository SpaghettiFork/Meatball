/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MEATBALL_NATIVE_CLIENT_H
#define MEATBALL_NATIVE_CLIENT_H

#include <xorg-server.h>

#include <dixstruct.h>
#include <stdint.h>
#include <wayland-server-core.h>

#define MB_CLIENT_OUTPUT_LIMIT (1024 * 1024)
#define MB_CLIENT_FD_LIMIT 64

typedef struct MbClient MbClient;

struct MbClient
{
    struct wl_list link;
    uint64_t lifetime;
    struct wl_client *wayland;
    struct wl_listener wayland_destroy;
    ClientPtr native;
    Bool failed;
    unsigned char *bytes;
    size_t length;
    size_t capacity;
    int fds[MB_CLIENT_FD_LIMIT];
    size_t fd_count;
    /* Message storage is borrowed until the handler returns. FD ownership
     * transfers separately through mb_client_take_fd.
     */
    void (*output) (MbClient *client, const void *message, size_t size);
    void *data;
};

extern Bool mb_clients_initialize (void);
extern void mb_clients_shutdown (void);
extern void mb_clients_check (void);
extern MbClient *mb_client_for_wayland (struct wl_client *wayland);
extern MbClient *mb_client_lookup (uint64_t lifetime);
extern int mb_client_take_fd (MbClient *client);
extern void mb_client_disconnect (MbClient *client);

#endif
