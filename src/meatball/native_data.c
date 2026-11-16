/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "server.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server-protocol.h>

#ifdef WL_DATA_DEVICE_MANAGER_RELEASE_SINCE_VERSION
#define DATA_VERSION WL_DATA_DEVICE_MANAGER_RELEASE_SINCE_VERSION
#else
#define DATA_VERSION 3
#endif
#define SOURCE_MIME_LIMIT 256
#define SOURCE_MIME_BYTES_LIMIT (64 * 1024)
#define MIME_LENGTH_LIMIT 4096
/* This error is absent from older Wayland SDK headers. */
#define DATA_DEVICE_USED_SOURCE 1
#define DATA_ACTIONS                                                          \
    (WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY |                                 \
     WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE |                                 \
     WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK)

enum SourceUse
{
    SOURCE_UNUSED,
    SOURCE_SELECTION,
    SOURCE_DRAG,
};

struct DataSource
{
    struct wl_list link;
    struct wl_resource *resource;
    char **mimes;
    size_t mime_count, mime_bytes;
    enum SourceUse use;
    uint32_t actions;
    Bool actions_set, cancelled;
};

struct DataSeat
{
    struct wl_list link;
    uint64_t id, focus;
    Bool removed, serial_set;
    uint32_t selection_serial;
    struct DataSource *selection;
};

struct DataDevice
{
    struct wl_list link;
    struct wl_resource *resource;
    struct DataSeat *seat;
    MbClient *client;
};

struct DataOffer
{
    struct wl_list link;
    struct wl_resource *resource;
    struct DataSource *source;
    uint64_t seat, client;
    Bool active;
};

struct MbData
{
    struct wl_global *global;
    struct wl_list seats, devices, sources, offers;
};

static void send_selection (struct DataDevice *device);

static void
release (struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_destroy (resource);
}

static Bool
has_mime (struct DataSource *source, const char *mime)
{
    size_t i;

    for (i = 0; i < source->mime_count; i++)
        if (!strcmp (source->mimes[i], mime))
            return TRUE;
    return FALSE;
}

static void
invalidate_selection (struct DataSeat *seat)
{
    struct DataOffer *offer;

    wl_list_for_each (offer, &compositor.data->offers, link)
    {
        if (offer->seat == seat->id)
            offer->active = FALSE;
    }
}

static void
announce_selection (struct DataSeat *seat)
{
    struct DataDevice *device;

    wl_list_for_each (device, &compositor.data->devices, link)
    {
        if (device->seat == seat && !device->client->failed &&
            device->client->lifetime == seat->focus)
            send_selection (device);
    }
}

static void
source_destroyed (struct wl_resource *resource)
{
    struct DataSource *source = wl_resource_get_user_data (resource);
    struct DataOffer *offer;
    struct DataSeat *seat;
    size_t i;

    wl_list_for_each (offer, &compositor.data->offers, link)
    {
        if (offer->source == source)
        {
            offer->source = NULL;
            offer->active = FALSE;
        }
    }
    wl_list_for_each (seat, &compositor.data->seats, link)
    {
        if (seat->selection == source)
        {
            seat->selection = NULL;
            invalidate_selection (seat);
            announce_selection (seat);
        }
    }
    for (i = 0; i < source->mime_count; i++)
        free (source->mimes[i]);
    free (source->mimes);
    wl_list_remove (&source->link);
    free (source);
}

static void
source_offer (struct wl_client *client, struct wl_resource *resource,
              const char *mime)
{
    struct DataSource *source = wl_resource_get_user_data (resource);
    size_t size = strlen (mime) + 1;
    char **mimes, *copy;

    if (source->use != SOURCE_UNUSED)
    {
        wl_resource_post_error (resource, WL_DATA_SOURCE_ERROR_INVALID_SOURCE,
                                "data source has already been used");
        return;
    }
    if (has_mime (source, mime))
        return;
    if (source->mime_count >= SOURCE_MIME_LIMIT || size > MIME_LENGTH_LIMIT ||
        size > SOURCE_MIME_BYTES_LIMIT - source->mime_bytes)
    {
        wl_client_post_implementation_error (
            client, "data source exceeds MIME limits");
        return;
    }
    copy = strdup (mime);
    if (!copy)
        goto fail;
    mimes =
        realloc (source->mimes, (source->mime_count + 1) * sizeof (*mimes));
    if (!mimes)
    {
        free (copy);
        goto fail;
    }
    source->mimes = mimes;
    source->mimes[source->mime_count++] = copy;
    source->mime_bytes += size;
    return;

fail:
    wl_resource_post_no_memory (resource);
}

static void
source_actions (struct wl_client *client, struct wl_resource *resource,
                uint32_t actions)
{
    struct DataSource *source = wl_resource_get_user_data (resource);

    if (actions & ~DATA_ACTIONS)
    {
        wl_resource_post_error (resource,
                                WL_DATA_SOURCE_ERROR_INVALID_ACTION_MASK,
                                "invalid source action mask");
        return;
    }
    if (source->actions_set || source->use != SOURCE_UNUSED)
    {
        wl_resource_post_error (resource, WL_DATA_SOURCE_ERROR_INVALID_SOURCE,
                                "source actions must be set once before use");
        return;
    }
    source->actions = actions;
    source->actions_set = TRUE;
}

static const struct wl_data_source_interface source_implementation = {
    .offer = source_offer,
    .destroy = release,
    .set_actions = source_actions,
};

static void
create_source (struct wl_client *client, struct wl_resource *resource,
               uint32_t id)
{
    struct DataSource *source = calloc (1, sizeof (*source));

    if (!source)
        goto fail;
    source->resource =
        wl_resource_create (client, &wl_data_source_interface,
                            wl_resource_get_version (resource), id);
    if (!source->resource)
    {
        free (source);
        goto fail;
    }
    wl_list_insert (compositor.data->sources.prev, &source->link);
    wl_resource_set_implementation (source->resource, &source_implementation,
                                    source, source_destroyed);
    return;

fail:
    wl_resource_post_no_memory (resource);
}

static void
selection_accept (struct wl_client *client, struct wl_resource *resource,
                  uint32_t serial, const char *mime)
{
}

static void
selection_receive (struct wl_client *client, struct wl_resource *resource,
                   const char *mime, int32_t fd)
{
    struct DataOffer *offer = wl_resource_get_user_data (resource);

    if (offer->active && offer->source && has_mime (offer->source, mime))
        wl_data_source_send_send (offer->source->resource, mime, fd);
    close (fd);
}

static void
selection_finish (struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_post_error (resource, WL_DATA_OFFER_ERROR_INVALID_FINISH,
                            "selection offers cannot be finished");
}

static void
selection_actions (struct wl_client *client, struct wl_resource *resource,
                   uint32_t actions, uint32_t preferred)
{
    wl_resource_post_error (resource, WL_DATA_OFFER_ERROR_INVALID_OFFER,
                            "selection offers do not negotiate drag actions");
}

static void
offer_destroyed (struct wl_resource *resource)
{
    struct DataOffer *offer = wl_resource_get_user_data (resource);

    wl_list_remove (&offer->link);
    free (offer);
}

static const struct wl_data_offer_interface selection_implementation = {
    .accept = selection_accept,
    .receive = selection_receive,
    .destroy = release,
    .finish = selection_finish,
    .set_actions = selection_actions,
};

static void
send_selection (struct DataDevice *device)
{
    struct DataSource *source = device->seat->selection;
    struct DataOffer *offer;
    size_t i;

    if (!source || device->seat->removed)
    {
        wl_data_device_send_selection (device->resource, NULL);
        return;
    }
    offer = calloc (1, sizeof (*offer));
    if (!offer)
        goto fail;
    offer->resource =
        wl_resource_create (device->client->wayland, &wl_data_offer_interface,
                            wl_resource_get_version (device->resource), 0);
    if (!offer->resource)
    {
        free (offer);
        goto fail;
    }
    offer->source = source;
    offer->seat = device->seat->id;
    offer->client = device->client->lifetime;
    offer->active = TRUE;
    wl_list_insert (compositor.data->offers.prev, &offer->link);
    wl_resource_set_implementation (offer->resource, &selection_implementation,
                                    offer, offer_destroyed);
    wl_data_device_send_data_offer (device->resource, offer->resource);
    for (i = 0; i < source->mime_count; i++)
        wl_data_offer_send_offer (offer->resource, source->mimes[i]);
    wl_data_device_send_selection (device->resource, offer->resource);
    return;

fail:
    wl_resource_post_no_memory (device->resource);
}

static void
set_selection (struct wl_client *client, struct wl_resource *resource,
               struct wl_resource *source_resource, uint32_t serial)
{
    struct DataDevice *device = wl_resource_get_user_data (resource);
    struct DataSeat *seat = device->seat;
    struct DataSource *source =
        source_resource ? wl_resource_get_user_data (source_resource) : NULL;

    if (seat->removed ||
        !mb_input_selection_serial (seat->id, device->client, serial) ||
        (seat->serial_set &&
         (uint32_t)(serial - seat->selection_serial) > INT32_MAX))
        return;
    if (source && source->use != SOURCE_UNUSED)
    {
        wl_resource_post_error (resource, DATA_DEVICE_USED_SOURCE,
                                "data source has already been used");
        return;
    }
    if (source && source->actions_set)
    {
        wl_resource_post_error (source_resource,
                                WL_DATA_SOURCE_ERROR_INVALID_SOURCE,
                                "drag source cannot own the selection");
        return;
    }
    if (seat->selection)
    {
        seat->selection->cancelled = TRUE;
        wl_data_source_send_cancelled (seat->selection->resource);
    }
    invalidate_selection (seat);
    seat->selection = source;
    seat->selection_serial = serial;
    seat->serial_set = TRUE;
    if (source)
        source->use = SOURCE_SELECTION;
    announce_selection (seat);
}

static void
start_drag (struct wl_client *client, struct wl_resource *resource,
            struct wl_resource *source, struct wl_resource *origin,
            struct wl_resource *icon, uint32_t serial)
{
}

static void
device_destroyed (struct wl_resource *resource)
{
    struct DataDevice *device = wl_resource_get_user_data (resource);

    wl_list_remove (&device->link);
    free (device);
}

static const struct wl_data_device_interface device_implementation = {
    .start_drag = start_drag,
    .set_selection = set_selection,
    .release = release,
};

static void
get_device (struct wl_client *client, struct wl_resource *resource,
            uint32_t id, struct wl_resource *seat_resource)
{
    uint64_t seat_id = mb_input_seat_id (seat_resource);
    struct DataSeat *seat;
    struct DataDevice *device;

    if (!seat_id)
    {
        wl_client_post_implementation_error (client,
                                             "invalid data device seat");
        return;
    }
    wl_list_for_each (seat, &compositor.data->seats, link)
    {
        if (seat->id == seat_id)
            break;
    }
    if (&seat->link == &compositor.data->seats)
    {
        MbClient *focus = mb_input_keyboard_client (seat_id);

        seat = calloc (1, sizeof (*seat));
        if (!seat)
            goto fail;
        seat->id = seat_id;
        seat->focus = focus ? focus->lifetime : 0;
        wl_list_insert (compositor.data->seats.prev, &seat->link);
    }
    device = calloc (1, sizeof (*device));
    if (!device)
        goto fail;
    device->resource =
        wl_resource_create (client, &wl_data_device_interface,
                            wl_resource_get_version (resource), id);
    if (!device->resource)
    {
        free (device);
        goto fail;
    }
    device->seat = seat;
    device->client = mb_client_for_wayland (client);
    wl_list_insert (compositor.data->devices.prev, &device->link);
    wl_resource_set_implementation (device->resource, &device_implementation,
                                    device, device_destroyed);
    if (seat->focus == device->client->lifetime && !seat->removed)
        send_selection (device);
    return;

fail:
    wl_resource_post_no_memory (resource);
}

static const struct wl_data_device_manager_interface implementation = {
    .create_data_source = create_source,
    .get_data_device = get_device,
#ifdef WL_DATA_DEVICE_MANAGER_RELEASE_SINCE_VERSION
    .release = release,
#endif
};

static void
bind_manager (struct wl_client *client, void *data, uint32_t version,
              uint32_t id)
{
    struct wl_resource *resource = wl_resource_create (
        client, &wl_data_device_manager_interface, version, id);

    if (!resource)
    {
        wl_client_post_no_memory (client);
        return;
    }
    wl_resource_set_implementation (resource, &implementation, NULL, NULL);
}

void
mb_data_keyboard_focus (uint64_t id, MbClient *client)
{
    struct DataSeat *seat;
    uint64_t focus = client ? client->lifetime : 0;

    if (!compositor.data)
        return;
    wl_list_for_each (seat, &compositor.data->seats, link)
    {
        if (seat->id != id || seat->removed || seat->focus == focus)
            continue;
        invalidate_selection (seat);
        seat->focus = focus;
        announce_selection (seat);
    }
}

void
mb_data_seat_removed (uint64_t id)
{
    struct DataSeat *seat;

    if (!compositor.data)
        return;
    wl_list_for_each (seat, &compositor.data->seats, link)
    {
        if (seat->id != id)
            continue;
        if (seat->selection)
        {
            seat->selection->cancelled = TRUE;
            wl_data_source_send_cancelled (seat->selection->resource);
            seat->selection = NULL;
        }
        seat->focus = 0;
        seat->removed = TRUE;
        invalidate_selection (seat);
    }
}

Bool
mb_data_initialize (void)
{
    struct MbData *data = calloc (1, sizeof (*data));

    if (!data)
        return FALSE;
    wl_list_init (&data->seats);
    wl_list_init (&data->devices);
    wl_list_init (&data->sources);
    wl_list_init (&data->offers);
    compositor.data = data;
    data->global = wl_global_create (compositor.wl_display,
                                     &wl_data_device_manager_interface,
                                     DATA_VERSION, NULL, bind_manager);
    if (!data->global)
    {
        mb_data_shutdown ();
        return FALSE;
    }
    return TRUE;
}

void
mb_data_shutdown (void)
{
    struct MbData *data = compositor.data;
    struct DataDevice *device, *next_device;
    struct DataSource *source, *next_source;
    struct DataOffer *offer, *next_offer;
    struct DataSeat *seat, *next_seat;

    if (!data)
        return;
    if (data->global)
        wl_global_destroy (data->global);
    wl_list_for_each_safe (device, next_device, &data->devices, link)
    {
        wl_resource_destroy (device->resource);
    }
    wl_list_for_each_safe (offer, next_offer, &data->offers, link)
    {
        wl_resource_destroy (offer->resource);
    }
    wl_list_for_each_safe (source, next_source, &data->sources, link)
    {
        wl_resource_destroy (source->resource);
    }
    wl_list_for_each_safe (seat, next_seat, &data->seats, link)
    {
        wl_list_remove (&seat->link);
        free (seat);
    }
    free (data);
    compositor.data = NULL;
}
