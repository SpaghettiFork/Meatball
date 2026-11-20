/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "server.h"

#include <X11/Xproto.h>
#include <errno.h>
#include <external.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

#define MESSAGES_PER_CLIENT 32
#define MESSAGES_PER_DISPATCH 256
#define SEND_EVENT_MASK 0x80
#define X11_WORD_BYTES 4
#define NO_FD (-1)

static uint64_t next_lifetime;

static void
schedule_clients (void)
{
    const uint64_t wakeup = 1;
    ssize_t count;

    if (!compositor.clients_initialized || compositor.clients_stopping ||
        compositor.clients_fd == NO_FD)
        return;
    do
        count = write (compositor.clients_fd, &wakeup, sizeof (wakeup));
    while (count < 0 && errno == EINTR);
}

void
mb_client_disconnect (MbClient *client)
{
    client->failed = TRUE;
    schedule_clients ();
}

static int
client_write (ClientPtr native, int count, const void *data, void *closure)
{
    MbClient *client = closure;
    size_t needed;

    if (client->failed || count < 0 ||
        (size_t)count > MB_CLIENT_OUTPUT_LIMIT - client->length)
        goto fail;
    needed = client->length + count;
    if (needed > client->capacity)
    {
        size_t capacity =
            client->capacity ? client->capacity : sz_xGenericReply;
        unsigned char *bytes;

        while (capacity < needed)
            capacity *= 2;
        bytes = realloc (client->bytes, capacity);
        if (!bytes)
            goto fail;
        client->bytes = bytes;
        client->capacity = capacity;
    }
    if (count)
        memcpy (client->bytes + client->length, data, count);
    client->length = needed;
    if (!mb_input_observe_write (client, data, count))
        goto fail;
    schedule_clients ();
    return count;

fail:
    mb_client_disconnect (client);
    return -1;
}

static int
client_send_fd (ClientPtr native, int fd, void *closure)
{
    MbClient *client = closure;
    int flags;

    if (client->failed || client->fd_count == MB_CLIENT_FD_LIMIT)
        goto fail;
    flags = fcntl (fd, F_GETFD);
    if (flags < 0 || fcntl (fd, F_SETFD, flags | FD_CLOEXEC) < 0)
        goto fail;
    client->fds[client->fd_count++] = fd;
    schedule_clients ();
    return 0;

fail:
    mb_client_disconnect (client);
    return -1;
}

static void
client_closed (ClientPtr native, void *closure)
{
    MbClient *client = closure;

    client->native = NULL;
    mb_client_disconnect (client);
}

static void
wayland_closed (struct wl_listener *listener, void *data)
{
    MbClient *client = wl_container_of (listener, client, wayland_destroy);

    wl_list_remove (&client->wayland_destroy.link);
    client->wayland = NULL;
    if (client->native)
        CloseDownClient (client->native);
    mb_client_disconnect (client);
}

static MbClient *
create_client (struct wl_client *wayland)
{
    static const ExternalClientCallbacks callbacks = {
        .write = client_write,
        .send_fd = client_send_fd,
        .closed = client_closed,
    };
    LocalClientCredRec credentials = {
        .fieldsSet = LCC_UID_SET | LCC_GID_SET | LCC_PID_SET,
    };
    MbClient *client;

    if (next_lifetime == UINT64_MAX)
        return NULL;
    client = calloc (1, sizeof (*client));
    if (!client)
        return NULL;
    client->lifetime = ++next_lifetime;
    client->wayland = wayland;
    wl_list_insert (compositor.clients.prev, &client->link);
    if (wayland)
    {
        pid_t pid;
        uid_t uid;
        gid_t gid;

        wl_client_get_credentials (wayland, &pid, &uid, &gid);
        credentials.pid = pid;
        credentials.euid = uid;
        credentials.egid = gid;
        client->wayland_destroy.notify = wayland_closed;
        wl_client_add_destroy_listener (wayland, &client->wayland_destroy);
    }
    else
    {
        credentials.pid = getpid ();
        credentials.euid = geteuid ();
        credentials.egid = getegid ();
    }
    client->native =
        dixCreateExternalClient (&callbacks, &credentials, client);
    if (!client->native)
    {
        mb_client_disconnect (client);
        return NULL;
    }
    return client;
}

static void
wayland_created (struct wl_listener *listener, void *data)
{
    struct wl_client *wayland = data;

    if (!create_client (wayland))
        wl_client_post_no_memory (wayland);
}

MbClient *
mb_client_for_wayland (struct wl_client *wayland)
{
    struct wl_listener *listener =
        wl_client_get_destroy_listener (wayland, wayland_closed);
    MbClient *client;

    if (!listener)
        return NULL;
    client = wl_container_of (listener, client, wayland_destroy);
    return client->native && !client->failed ? client : NULL;
}

MbClient *
mb_client_lookup (uint64_t lifetime)
{
    MbClient *client;

    if (!compositor.clients_initialized)
        return NULL;
    wl_list_for_each (client, &compositor.clients, link)
    {
        if (client->lifetime == lifetime && client->native && !client->failed)
            return client;
    }
    return NULL;
}

int
mb_client_take_fd (MbClient *client)
{
    int fd;

    if (!client->fd_count || client->failed)
        return NO_FD;
    fd = client->fds[0];
    memmove (client->fds, client->fds + 1,
             --client->fd_count * sizeof (*client->fds));
    return fd;
}

void
mb_clients_check (void)
{
    MbClient *client;

    if (!compositor.clients_initialized)
        return;
    wl_list_for_each (client, &compositor.clients, link)
    {
        if (client->native && client->native->noClientException != Success &&
            !client->failed)
            mb_client_disconnect (client);
    }
}

static void
release_client (MbClient *client)
{
    size_t i;

    if (client->wayland)
        wl_client_destroy (client->wayland);
    if (client->native)
        CloseDownClient (client->native);
    for (i = 0; i < client->fd_count; i++)
        close (client->fds[i]);
    free (client->bytes);
    wl_list_remove (&client->link);
    free (client);
}

/* Zero means an incomplete message. Oversized headers fail before allocation.
 * Native clients use the server's byte order, including XKB and DRI3 replies.
 */
static size_t
message_size (MbClient *client)
{
    xGenericReply header;
    size_t size = sz_xGenericReply;
    unsigned int type;

    if (client->length < sizeof (header))
        return 0;
    memcpy (&header, client->bytes, sizeof (header));
    type = header.type & ~SEND_EVENT_MASK;
    if (type == X_Reply || type == GenericEvent)
    {
        if (header.length > (MB_CLIENT_OUTPUT_LIMIT - size) / X11_WORD_BYTES)
        {
            mb_client_disconnect (client);
            return 0;
        }
        size += header.length * X11_WORD_BYTES;
    }
    return client->length >= size ? size : 0;
}

static void
clients_ready (int fd, int ready, void *data)
{
    uint64_t wakeup;
    ssize_t count;
    unsigned int remaining = MESSAGES_PER_DISPATCH;
    int clients_left = wl_list_length (&compositor.clients);

    do
        count = read (fd, &wakeup, sizeof (wakeup));
    while (count < 0 && errno == EINTR);
    if (ready & X_NOTIFY_ERROR)
    {
        mb_log (X_ERROR, "meatball: Native client wakeup fd failed\n");
        meatball_shutdown ();
        return;
    }
    while (clients_left-- && remaining && !wl_list_empty (&compositor.clients))
    {
        MbClient *client =
            wl_container_of (compositor.clients.next, client, link);
        unsigned int budget = MESSAGES_PER_CLIENT;
        size_t size;

        wl_list_remove (&client->link);
        wl_list_insert (compositor.clients.prev, &client->link);
        if (client->native && client->native->noClientException != Success)
            client->failed = TRUE;
        while (!client->failed && budget-- && remaining &&
               (size = message_size (client)))
        {
            unsigned char *message = malloc (size);

            if (!message)
            {
                mb_client_disconnect (client);
                break;
            }
            memcpy (message, client->bytes, size);
            client->length -= size;
            memmove (client->bytes, client->bytes + size, client->length);
            remaining--;
            if ((client != compositor.bridge_client ||
                 !mb_dmabuf_dri3_reply (client, message, size)) &&
                client->output)
                client->output (client, message, size);
            free (message);
        }
        if (client->failed)
        {
            if (client == compositor.bridge_client)
            {
                mb_log (X_ERROR, "meatball: Native bridge client closed\n");
                meatball_shutdown ();
                return;
            }
            release_client (client);
        }
        else if (message_size (client))
            schedule_clients ();
    }
    if (!remaining)
        schedule_clients ();
}

Bool
mb_clients_initialize (void)
{
    wl_list_init (&compositor.clients);
    compositor.clients_fd = NO_FD;
    compositor.clients_initialized = TRUE;
    compositor.clients_fd = eventfd (0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (compositor.clients_fd == NO_FD ||
        !SetNotifyFd (compositor.clients_fd, clients_ready, X_NOTIFY_READ,
                      NULL))
        return FALSE;
    compositor.bridge_client = create_client (NULL);
    if (!compositor.bridge_client)
        return FALSE;
    compositor.client_created.notify = wayland_created;
    wl_display_add_client_created_listener (compositor.wl_display,
                                            &compositor.client_created);
    return TRUE;
}

void
mb_clients_shutdown (void)
{
    MbClient *client, *next;

    if (!compositor.clients_initialized)
        return;
    compositor.clients_stopping = TRUE;
    if (compositor.client_created.notify)
    {
        wl_list_remove (&compositor.client_created.link);
        compositor.client_created.notify = NULL;
    }
    if (compositor.clients_fd != NO_FD)
    {
        RemoveNotifyFd (compositor.clients_fd);
        close (compositor.clients_fd);
        compositor.clients_fd = NO_FD;
    }
    wl_list_for_each_safe (client, next, &compositor.clients, link)
        release_client (client);
    compositor.bridge_client = NULL;
    compositor.clients_initialized = FALSE;
}
