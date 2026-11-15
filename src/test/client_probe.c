/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "probe.h"

#include <X11/Xproto.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pixmapstr.h>
#include <resource.h>
#include <scrnintstr.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server-protocol.h>

#define TEST_INTERFACE "meatball_client_test"
#define TEST_VERSION 1
#define RESULT_EVENT 0
#define PAYLOAD_WORD 0x12345678
#define RESULT_OWNERSHIP 1
#define RESULT_OUTPUT 2
#define RESULT_LOOKUP 3
#define RESULT_FD_CLOSED 4
#define EXERCISE_OWNERSHIP 1
#define EXERCISE_OUTPUT 2
#define EXERCISE_OVERSIZED 3
#define EXERCISE_OVERFLOW 4
#define EXERCISE_EXCEPTION 5
#define EXERCISE_ORPHAN_FD 6
#define EXERCISE_LOOKUP 7
#define EXERCISE_FD_CLOSED 8
#define EXERCISE_NATIVE_CLOSE 9
#define EXERCISE_CLOSE_IN_OUTPUT 10
#define EXERCISE_FD_OVERFLOW 11
#define EXERCISE_WINDOW 12
#define EXERCISE_DESTROY_WINDOW 13
#define RESULT_WINDOW_DESTROYED 5
#define OUTPUT_STAGES 5
#define U32_BITS 32

struct TestClient
{
    MbClient *client;
    struct wl_resource *resource;
    unsigned int outputs;
    Bool discard_output;
    Bool close_in_output;
    MbWindow window;
};

static Bool inside_request;
static int orphan_fd = -1;
static struct wl_global *test_global;

static void
output (MbClient *client, const void *message, size_t size)
{
    struct TestClient *test = client->data;
    const xGenericReply *header = message;
    unsigned int stage = ++test->outputs;

    assert (!inside_request);
    assert (!test->discard_output);
    assert (header->data1 == stage);
    if (test->close_in_output)
    {
        uint64_t lifetime = client->lifetime;

        CloseDownClient (client->native);
        assert (!client->native && !mb_client_lookup (lifetime));
        assert (header->data1 == stage);
        return;
    }
    if (stage == 1 || stage == OUTPUT_STAGES)
        assert (header->type == ClientMessage && size == sz_xEvent);
    else if (stage == 2)
    {
        assert (header->type == GenericEvent);
        assert (size == sizeof (xGenericEvent) + 2 * sizeof (CARD32));
    }
    else if (stage == 3)
    {
        CARD32 payload;

        assert (header->type == X_Reply &&
                size == sz_xGenericReply + sizeof (payload));
        memcpy (&payload, (const char *)message + sz_xGenericReply,
                sizeof (payload));
        assert (payload == PAYLOAD_WORD);
    }
    else if (stage == 4)
    {
        int fd = mb_client_take_fd (client);

        assert (header->type == X_Reply && fd >= 0);
        assert (fcntl (fd, F_GETFD) & FD_CLOEXEC);
        close (fd);
        assert (mb_client_take_fd (client) == -1);
    }
    else
        assert (FALSE);
    if (stage == 1)
    {
        xGenericReply event = { .type = ClientMessage,
                                .data1 = OUTPUT_STAGES };

        assert (WriteToClient (client->native, sizeof (event), &event) >= 0);
    }
    wl_resource_post_event (test->resource, RESULT_EVENT, RESULT_OUTPUT, stage,
                            0, 0);
}

static void
exercise (struct wl_client *wayland, struct wl_resource *resource,
          uint32_t operation, uint32_t high, uint32_t low)
{
    struct TestClient *test = wl_resource_get_user_data (resource);
    MbClient *client = test->client;

    assert (client->native && mb_client_for_wayland (wayland) == client);
    inside_request = TRUE;
    switch (operation)
    {
    case EXERCISE_OWNERSHIP:
    {
        LocalClientCredRec *credentials;
        pid_t pid;
        uid_t uid;
        gid_t gid;
        ScreenPtr screen = screenInfo.screens[0];
        PixmapPtr pixmap =
            screen->CreatePixmap (screen, 1, 1, screen->rootDepth, 0);
        XID id = FakeClientID (client->native->index);

        assert (pixmap && LegalNewID (id, client->native));
        pixmap->drawable.id = id;
        assert (AddResource (id, RT_PIXMAP, pixmap));
        wl_client_get_credentials (wayland, &pid, &uid, &gid);
        assert (GetLocalClientCreds (client->native, &credentials) == Success);
        assert (credentials->pid == pid && credentials->euid == uid &&
                credentials->egid == gid);
        FreeLocalClientCreds (credentials);
        wl_resource_post_event (resource, RESULT_EVENT, RESULT_OWNERSHIP, id,
                                (uint32_t)(client->lifetime >> U32_BITS),
                                (uint32_t)client->lifetime);
        break;
    }
    case EXERCISE_OUTPUT:
    {
        xGenericReply event = { .type = ClientMessage, .data1 = 1 };
        xGenericReply generic = { .type = GenericEvent,
                                  .data1 = 2,
                                  .length = 2 };
        xGenericReply reply = { .type = X_Reply, .data1 = 3, .length = 1 };
        xGenericReply fd_reply = { .type = X_Reply, .data1 = 4 };
        CARD32 payload[2] = { PAYLOAD_WORD, PAYLOAD_WORD };
        int fd = open ("/dev/null", O_RDONLY);

        assert (fd >= 0);
        client->output = output;
        assert (WriteToClient (client->native, sizeof (event), &event) >= 0);
        assert (WriteToClient (client->native, sizeof (generic), &generic) >=
                0);
        assert (WriteToClient (client->native, sizeof (payload), payload) >=
                0);
        assert (WriteToClient (client->native, sizeof (reply) / 2, &reply) >=
                0);
        assert (WriteToClient (client->native, sizeof (reply) / 2,
                               (const char *)&reply + sizeof (reply) / 2) >=
                0);
        assert (WriteToClient (client->native, sizeof (*payload), payload) >=
                0);
        assert (WriteFdToClient (client->native, fd, TRUE) == 0);
        assert (WriteToClient (client->native, sizeof (fd_reply), &fd_reply) >=
                0);
        assert (!test->outputs);
        break;
    }
    case EXERCISE_OVERSIZED:
    {
        xGenericReply reply = { .type = X_Reply, .length = UINT32_MAX };

        assert (WriteToClient (client->native, sizeof (reply), &reply) >= 0);
        break;
    }
    case EXERCISE_OVERFLOW:
    {
        const char unused = 0;

        assert (WriteToClient (client->native, MB_CLIENT_OUTPUT_LIMIT + 1,
                               &unused) < 0);
        assert (client->native->noClientException != Success);
        break;
    }
    case EXERCISE_EXCEPTION:
        MarkClientException (client->native);
        break;
    case EXERCISE_ORPHAN_FD:
        orphan_fd = open ("/dev/null", O_RDONLY);
        assert (orphan_fd >= 0);
        assert (WriteFdToClient (client->native, orphan_fd, TRUE) == 0);
        MarkClientException (client->native);
        break;
    case EXERCISE_LOOKUP:
        wl_resource_post_event (
            resource, RESULT_EVENT, RESULT_LOOKUP,
            mb_client_lookup (((uint64_t)high << U32_BITS) | low) != NULL, 0,
            0);
        break;
    case EXERCISE_FD_CLOSED:
        assert (orphan_fd >= 0);
        errno = 0;
        assert (fcntl (orphan_fd, F_GETFD) == -1 && errno == EBADF);
        wl_resource_post_event (resource, RESULT_EVENT, RESULT_FD_CLOSED, 0, 0,
                                0);
        orphan_fd = -1;
        break;
    case EXERCISE_NATIVE_CLOSE:
    case EXERCISE_CLOSE_IN_OUTPUT:
    {
        xEvent event = { .u.u = { .type = ClientMessage, .detail = 1 } };
        uint64_t lifetime = client->lifetime;

        client->output = output;
        test->discard_output = operation == EXERCISE_NATIVE_CLOSE;
        test->close_in_output = operation == EXERCISE_CLOSE_IN_OUTPUT;
        WriteEventsToClient (client->native, 1, &event);
        if (operation == EXERCISE_NATIVE_CLOSE)
        {
            CloseDownClient (client->native);
            assert (!client->native && !mb_client_lookup (lifetime));
        }
        break;
    }
    case EXERCISE_FD_OVERFLOW:
    {
        unsigned int i;

        for (i = 0; i <= MB_CLIENT_FD_LIMIT; i++)
        {
            int fd = open ("/dev/null", O_RDONLY);
            int result;

            assert (fd >= 0);
            result = WriteFdToClient (client->native, fd, TRUE);
            if (i == MB_CLIENT_FD_LIMIT)
            {
                assert (result < 0 && fcntl (fd, F_GETFD) >= 0);
                close (fd);
            }
            else
                assert (result == 0);
        }
        break;
    }
    case EXERCISE_WINDOW:
        assert (
            mb_window_create (&test->window, client, screenInfo.screens[0]));
        assert (mb_window_get (&test->window, DixGetAttrAccess));
        wl_resource_post_event (resource, RESULT_EVENT, RESULT_OWNERSHIP,
                                test->window.id,
                                (uint32_t)(client->lifetime >> U32_BITS),
                                (uint32_t)client->lifetime);
        break;
    case EXERCISE_DESTROY_WINDOW:
        mb_window_destroy (&test->window);
        assert (!test->window.id && mb_client_lookup (client->lifetime));
        wl_resource_post_event (resource, RESULT_EVENT,
                                RESULT_WINDOW_DESTROYED, 0, 0, 0);
        break;
    default:
        assert (FALSE);
    }
    inside_request = FALSE;
}

static const struct wl_message requests[] = { { "exercise", "uuu", NULL } };
static const struct wl_message events[] = { { "result", "uuuu", NULL } };
static const struct wl_interface interface = {
    TEST_INTERFACE, TEST_VERSION,        ARRAY_SIZE (requests),
    requests,       ARRAY_SIZE (events), events,
};
static const struct
{
    __typeof__ (&exercise) exercise;
} implementation = { exercise };

static void
resource_destroyed (struct wl_resource *resource)
{
    struct TestClient *test = wl_resource_get_user_data (resource);

    test->client->output = NULL;
    test->client->data = NULL;
    mb_window_destroy (&test->window);
    free (test);
}

static void
bind_test (struct wl_client *wayland, void *data, uint32_t version,
           uint32_t id)
{
    MbClient *client = mb_client_for_wayland (wayland);
    struct TestClient *test = calloc (1, sizeof (*test));

    assert (client && !client->data && test);
    test->client = client;
    test->resource =
        wl_resource_create (wayland, &interface, TEST_VERSION, id);
    assert (test->resource);
    client->data = test;
    wl_resource_set_implementation (test->resource, &implementation, test,
                                    resource_destroyed);
}

void
mb_probe_start (void)
{
    test_global = wl_global_create (compositor.wl_display, &interface,
                                    TEST_VERSION, NULL, bind_test);
    assert (test_global);
}

void
mb_probe_stop (void)
{
    if (test_global)
        wl_global_destroy (test_global);
    test_global = NULL;
}
