/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "primary-selection.h"
#include "server.h"

#include <X11/extensions/shapeconst.h>
#include <inputstr.h>
#include <picturestr.h>
#include <regionstr.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server-protocol.h>
#include <window.h>

#ifdef WL_DATA_DEVICE_MANAGER_RELEASE_SINCE_VERSION
#define DATA_VERSION WL_DATA_DEVICE_MANAGER_RELEASE_SINCE_VERSION
#else
#define DATA_VERSION 3
#endif
#define SOURCE_MIME_LIMIT 256
#define SOURCE_MIME_BYTES_LIMIT (64 * 1024)
#define MIME_LENGTH_LIMIT 4096
#define SAVE_TARGETS_ATOM_NAME "SAVE_TARGETS"
#define ICON_ALPHA_SHIFT 24
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

struct MbDataSource
{
    struct wl_list link;
    struct wl_resource *resource;
    char **mimes;
    size_t mime_count, mime_bytes;
    enum SourceUse use;
    uint32_t actions;
    Bool actions_set, cancelled, primary;
    const MbDataSourceOps *ops;
    void *data;
};

struct SeatSelection
{
    MbDataSource *source;
    uint32_t serial;
    Bool serial_set;
};

struct DataSeat
{
    struct wl_list link;
    uint64_t id, focus;
    Bool removed;
    struct SeatSelection selections[MB_SELECTION_COUNT];
    struct DataDrag *drag;
};

struct DataDevice
{
    struct wl_list link;
    struct wl_resource *resource;
    struct DataSeat *seat;
    MbClient *client;
    MbSelectionKind kind;
};

struct DataOffer
{
    struct wl_list link;
    struct wl_resource *resource;
    struct MbDataSource *source;
    uint64_t seat;
    MbSelectionKind kind;
    Bool active, drag_offer, finished;
    struct DataDrag *drag;
    struct DragTarget *target;
    const char *accepted;
    uint32_t actions, preferred, action;
    Bool action_sent;
};

struct DragTarget
{
    struct wl_list link;
    struct DataDevice *device;
    struct DataOffer *offer;
    Bool entered;
};

struct DataDrag
{
    struct wl_list link, targets;
    struct DataSeat *seat;
    struct DataDevice *origin_device;
    struct MbDataSource *source;
    MbSurface *origin, *icon, *focus;
    uint64_t origin_client;
    uint32_t enter_serial, source_action;
    Bool pointer_active, dropped, action_sent, ask;
    Bool external, native_target;
    struct DataOffer *winner;
    MbDragPosition position;
    int64_t icon_x, icon_y;
};

struct MbData
{
    struct wl_global *globals[MB_SELECTION_COUNT];
    struct wl_list seats, devices, sources, offers, drags;
    MbDataSource *imported[MB_SELECTION_COUNT];
    const MbDataBridge *bridge;
    void *bridge_data;
};

static void send_selection (struct DataDevice *device);
static void end_drag (struct DataDrag *drag, Bool success);
static void update_action (struct DataOffer *offer);

static void
release (struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_destroy (resource);
}

static const char *
find_mime (struct MbDataSource *source, const char *mime)
{
    size_t i;

    for (i = 0; i < source->mime_count; i++)
        if (!strcmp (source->mimes[i], mime))
            return source->mimes[i];
    return NULL;
}

static uint32_t
source_version (MbDataSource *source)
{
    return source->resource ? wl_resource_get_version (source->resource)
                            : DATA_VERSION;
}

static void
source_notify (MbDataSource *source, MbSourceEvent event, const char *mime,
               uint32_t action)
{
    if (source->ops && source->ops->notify)
    {
        source->ops->notify (source->data, event, mime, action);
        return;
    }
    if (!source->resource)
        return;
    switch (event)
    {
    case MB_SOURCE_TARGET:
        wl_data_source_send_target (source->resource, mime);
        break;
    case MB_SOURCE_ACTION:
        if (source_version (source) >= WL_DATA_SOURCE_ACTION_SINCE_VERSION)
            wl_data_source_send_action (source->resource, action);
        break;
    case MB_SOURCE_DROP:
        if (source_version (source) >=
            WL_DATA_SOURCE_DND_DROP_PERFORMED_SINCE_VERSION)
            wl_data_source_send_dnd_drop_performed (source->resource);
        break;
    case MB_SOURCE_FINISHED:
        if (source_version (source) >=
            WL_DATA_SOURCE_DND_FINISHED_SINCE_VERSION)
            wl_data_source_send_dnd_finished (source->resource);
        break;
    case MB_SOURCE_CANCELLED:
        if (source->primary)
            zwp_primary_selection_source_v1_send_cancelled (source->resource);
        else
            wl_data_source_send_cancelled (source->resource);
        break;
    case MB_SOURCE_DESTROYED:
        break;
    }
}

Bool
mb_data_source_send (MbDataSource *source, const char *mime, int fd)
{
    if (!source || !find_mime (source, mime))
    {
        close (fd);
        return FALSE;
    }
    if (source->ops)
    {
        source->ops->receive (source->data, mime, fd);
        return TRUE;
    }
    if (source->resource)
    {
        if (source->primary)
            zwp_primary_selection_source_v1_send_send (source->resource, mime,
                                                       fd);
        else
            wl_data_source_send_send (source->resource, mime, fd);
        close (fd);
        return TRUE;
    }
    close (fd);
    return FALSE;
}

const char *const *
mb_data_source_mimes (MbDataSource *source, size_t *count)
{
    *count = source->mime_count;
    return (const char *const *)source->mimes;
}

uint32_t
mb_data_source_actions (MbDataSource *source)
{
    return source->actions;
}

static void
invalidate_selection (struct DataSeat *seat, MbSelectionKind kind)
{
    struct DataOffer *offer;

    wl_list_for_each (offer, &compositor.data->offers, link)
    {
        if (offer->seat == seat->id && offer->kind == kind &&
            !offer->drag_offer)
            offer->active = FALSE;
    }
}

static void
announce_selection (struct DataSeat *seat, MbSelectionKind kind)
{
    struct DataDevice *device;

    wl_list_for_each (device, &compositor.data->devices, link)
    {
        if (device->seat == seat && device->kind == kind &&
            !device->client->failed && device->client->lifetime == seat->focus)
            send_selection (device);
    }
}

void
mb_data_source_destroy (MbDataSource *source)
{
    struct DataOffer *offer;
    struct DataSeat *seat;
    struct DataDrag *drag, *next_drag;
    size_t i;

    if (source->resource)
    {
        wl_resource_destroy (source->resource);
        return;
    }
    if (compositor.data->bridge)
        compositor.data->bridge->destroyed (compositor.data->bridge_data,
                                            source);
    for (MbSelectionKind kind = 0; kind < MB_SELECTION_COUNT; kind++)
        if (compositor.data->imported[kind] == source)
            compositor.data->imported[kind] = NULL;
    wl_list_for_each_safe (drag, next_drag, &compositor.data->drags, link)
    {
        if (drag->source == source)
            end_drag (drag, FALSE);
    }
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
        for (MbSelectionKind kind = 0; kind < MB_SELECTION_COUNT; kind++)
        {
            if (seat->selections[kind].source != source)
                continue;
            seat->selections[kind].source = NULL;
            invalidate_selection (seat, kind);
            announce_selection (seat, kind);
        }
    }
    source_notify (source, MB_SOURCE_DESTROYED, NULL, 0);
    for (i = 0; i < source->mime_count; i++)
        free (source->mimes[i]);
    free (source->mimes);
    wl_list_remove (&source->link);
    free (source);
}

static void
source_destroyed (struct wl_resource *resource)
{
    MbDataSource *source = wl_resource_get_user_data (resource);

    source->resource = NULL;
    wl_resource_set_user_data (resource, NULL);
    mb_data_source_destroy (source);
}

MbDataSource *
mb_data_source_create (const char *const *mimes, size_t count,
                       uint32_t actions, const MbDataSourceOps *ops,
                       void *data)
{
    MbDataSource *source;
    size_t i;

    if (!count || count > SOURCE_MIME_LIMIT || !ops || !ops->receive ||
        (actions & ~DATA_ACTIONS))
        return NULL;
    source = calloc (1, sizeof (*source));
    if (!source)
        return NULL;
    wl_list_insert (compositor.data->sources.prev, &source->link);
    source->mimes = calloc (count, sizeof (*source->mimes));
    if (!source->mimes)
        goto fail;
    for (i = 0; i < count; i++)
    {
        size_t size = strlen (mimes[i]) + 1;

        if (find_mime (source, mimes[i]))
            continue;
        if (size > MIME_LENGTH_LIMIT ||
            size > SOURCE_MIME_BYTES_LIMIT - source->mime_bytes)
            goto fail;
        source->mimes[source->mime_count] = strdup (mimes[i]);
        if (!source->mimes[source->mime_count])
            goto fail;
        source->mime_count++;
        source->mime_bytes += size;
    }
    source->actions = actions;
    source->actions_set = TRUE;
    source->ops = ops;
    source->data = data;
    return source;

fail:
    mb_data_source_destroy (source);
    return NULL;
}

void
mb_data_set_bridge (const MbDataBridge *bridge, void *data)
{
    compositor.data->bridge = bridge;
    compositor.data->bridge_data = data;
}

void
mb_data_import_selection (MbSelectionKind kind, MbDataSource *source)
{
    struct DataSeat *seat;
    MbDataSource *previous = compositor.data->imported[kind];

    compositor.data->imported[kind] = source;
    if (source)
        source->use = SOURCE_SELECTION;
    wl_list_for_each (seat, &compositor.data->seats, link)
    {
        if (seat->removed)
            continue;
        if (seat->selections[kind].source &&
            seat->selections[kind].source != source)
        {
            seat->selections[kind].source->cancelled = TRUE;
            source_notify (seat->selections[kind].source, MB_SOURCE_CANCELLED,
                           NULL, 0);
        }
        invalidate_selection (seat, kind);
        seat->selections[kind].source = source;
        announce_selection (seat, kind);
    }
    if (previous && previous != source)
        mb_data_source_destroy (previous);
}

static void
source_offer (struct wl_client *client, struct wl_resource *resource,
              const char *mime)
{
    struct MbDataSource *source = wl_resource_get_user_data (resource);
    size_t size = strlen (mime) + 1;
    char **mimes, *copy;

    if (source->use != SOURCE_UNUSED)
    {
        /* GTK 3 can append this clipboard persistence target after claiming
         * the selection. Keep the published MIME types as they were. */
        if (source->use == SOURCE_SELECTION && !source->primary &&
            !source->cancelled && !strcmp (mime, SAVE_TARGETS_ATOM_NAME))
            return;
        if (!source->primary)
            wl_resource_post_error (resource,
                                    WL_DATA_SOURCE_ERROR_INVALID_SOURCE,
                                    "data source has already been used");
        return;
    }
    if (find_mime (source, mime))
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
    struct MbDataSource *source = wl_resource_get_user_data (resource);

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

static const struct zwp_primary_selection_source_v1_interface
    primary_source = {
        .offer = source_offer,
        .destroy = release,
    };

static void
create_source (struct wl_client *client, struct wl_resource *resource,
               uint32_t id)
{
    struct MbDataSource *source = calloc (1, sizeof (*source));

    if (!source)
        goto fail;
    source->primary = wl_resource_get_user_data (resource) != NULL;
    source->resource = wl_resource_create (
        client,
        source->primary ? &zwp_primary_selection_source_v1_interface
                        : &wl_data_source_interface,
        wl_resource_get_version (resource), id);
    if (!source->resource)
    {
        free (source);
        goto fail;
    }
    wl_list_insert (compositor.data->sources.prev, &source->link);
    wl_resource_set_implementation (source->resource,
                                    source->primary
                                        ? (const void *)&primary_source
                                        : &source_implementation,
                                    source, source_destroyed);
    return;

fail:
    wl_resource_post_no_memory (resource);
}

static void
offer_accept (struct wl_client *client, struct wl_resource *resource,
              uint32_t serial, const char *mime)
{
    struct DataOffer *offer = wl_resource_get_user_data (resource);
    struct DataDrag *drag = offer->drag;

    if (offer->finished)
    {
        wl_resource_post_error (resource, WL_DATA_OFFER_ERROR_INVALID_OFFER,
                                "data offer has already been finished");
        return;
    }
    if (!offer->active || !drag || serial != drag->enter_serial)
        return;
    offer->accepted = mime ? find_mime (offer->source, mime) : NULL;
    drag->winner = offer;
    if (drag->source)
        source_notify (drag->source, MB_SOURCE_TARGET, offer->accepted, 0);
    update_action (offer);
}

static void
offer_receive (struct wl_client *client, struct wl_resource *resource,
               const char *mime, int32_t fd)
{
    struct DataOffer *offer = wl_resource_get_user_data (resource);

    if (offer->finished)
        wl_resource_post_error (resource, WL_DATA_OFFER_ERROR_INVALID_OFFER,
                                "data offer has already been finished");
    else if (offer->active && offer->source)
    {
        mb_data_source_send (offer->source, mime, fd);
        return;
    }
    close (fd);
}

static void
offer_finish (struct wl_client *client, struct wl_resource *resource)
{
    struct DataOffer *offer = wl_resource_get_user_data (resource);
    struct DataDrag *drag = offer->drag;

    if (!offer->active || !drag || !drag->dropped || drag->winner != offer ||
        !offer->accepted || !offer->action ||
        offer->action == WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK)
    {
        wl_resource_post_error (resource, WL_DATA_OFFER_ERROR_INVALID_FINISH,
                                "data offer cannot be finished in this state");
        return;
    }
    offer->finished = TRUE;
    if (drag->ask)
        source_notify (drag->source, MB_SOURCE_ACTION, NULL, offer->action);
    end_drag (drag, TRUE);
}

static void
offer_actions (struct wl_client *client, struct wl_resource *resource,
               uint32_t actions, uint32_t preferred)
{
    struct DataOffer *offer = wl_resource_get_user_data (resource);
    struct DataDrag *drag = offer->drag;

    if (!offer->drag_offer || offer->finished)
    {
        wl_resource_post_error (resource, WL_DATA_OFFER_ERROR_INVALID_OFFER,
                                "data offer does not negotiate drag actions");
        return;
    }
    if (actions & ~DATA_ACTIONS)
    {
        wl_resource_post_error (resource,
                                WL_DATA_OFFER_ERROR_INVALID_ACTION_MASK,
                                "invalid destination action mask");
        return;
    }
    if ((preferred && (preferred & (preferred - 1))) ||
        (preferred & ~actions) ||
        (drag && drag->dropped && drag->ask &&
         (preferred & ~drag->source->actions)))
    {
        wl_resource_post_error (resource, WL_DATA_OFFER_ERROR_INVALID_ACTION,
                                "invalid preferred drag action");
        return;
    }
    /* Motion can retire an offer before the destination receives leave.
     * An action reply to that offer no longer participates in negotiation. */
    if (!offer->active || !drag)
        return;
    if (drag->dropped && !drag->ask)
        return;
    offer->actions = actions;
    offer->preferred = preferred;
    drag->winner = offer;
    update_action (offer);
}

static void
offer_destroyed (struct wl_resource *resource)
{
    struct DataOffer *offer = wl_resource_get_user_data (resource);

    struct DataDrag *drag = offer->drag;

    if (offer->target)
        offer->target->offer = NULL;
    if (drag && drag->winner == offer)
    {
        drag->winner = NULL;
        if (drag->dropped)
            end_drag (drag, wl_resource_get_version (resource) <
                                WL_DATA_OFFER_FINISH_SINCE_VERSION);
        else if (drag->source)
            source_notify (drag->source, MB_SOURCE_TARGET, NULL, 0);
    }
    wl_list_remove (&offer->link);
    free (offer);
}

static const struct wl_data_offer_interface offer_implementation = {
    .accept = offer_accept,
    .receive = offer_receive,
    .destroy = release,
    .finish = offer_finish,
    .set_actions = offer_actions,
};

static const struct zwp_primary_selection_offer_v1_interface primary_offer = {
    .receive = offer_receive,
    .destroy = release,
};

static struct DataOffer *
create_offer (struct DataDevice *device, struct MbDataSource *source,
              struct DataDrag *drag)
{
    struct DataOffer *offer = calloc (1, sizeof (*offer));
    size_t i;
    Bool primary = device->kind == MB_SELECTION_PRIMARY;

    if (!offer)
        goto fail;
    offer->resource =
        wl_resource_create (device->client->wayland,
                            primary ? &zwp_primary_selection_offer_v1_interface
                                    : &wl_data_offer_interface,
                            wl_resource_get_version (device->resource), 0);
    if (!offer->resource)
    {
        free (offer);
        goto fail;
    }
    offer->source = source;
    offer->seat = device->seat->id;
    offer->kind = device->kind;
    offer->active = TRUE;
    offer->drag = drag;
    offer->drag_offer = drag != NULL;
    if (drag && wl_resource_get_version (offer->resource) <
                    WL_DATA_OFFER_SET_ACTIONS_SINCE_VERSION)
        offer->actions = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
    wl_list_insert (compositor.data->offers.prev, &offer->link);
    if (primary)
    {
        wl_resource_set_implementation (offer->resource, &primary_offer, offer,
                                        offer_destroyed);
        zwp_primary_selection_device_v1_send_data_offer (device->resource,
                                                         offer->resource);
        for (i = 0; i < source->mime_count; i++)
            zwp_primary_selection_offer_v1_send_offer (offer->resource,
                                                       source->mimes[i]);
        return offer;
    }
    wl_resource_set_implementation (offer->resource, &offer_implementation,
                                    offer, offer_destroyed);
    wl_data_device_send_data_offer (device->resource, offer->resource);
    for (i = 0; i < source->mime_count; i++)
        wl_data_offer_send_offer (offer->resource, source->mimes[i]);
    if (drag && wl_resource_get_version (offer->resource) >=
                    WL_DATA_OFFER_SOURCE_ACTIONS_SINCE_VERSION)
        wl_data_offer_send_source_actions (offer->resource, source->actions);
    return offer;

fail:
    wl_resource_post_no_memory (device->resource);
    return NULL;
}

static void
send_selection (struct DataDevice *device)
{
    MbDataSource *source = device->seat->selections[device->kind].source;
    struct DataOffer *offer = NULL;

    if (source && !device->seat->removed)
    {
        offer = create_offer (device, source, NULL);
        if (!offer)
            return;
    }
    if (device->kind == MB_SELECTION_PRIMARY)
        zwp_primary_selection_device_v1_send_selection (
            device->resource, offer ? offer->resource : NULL);
    else
        wl_data_device_send_selection (device->resource,
                                       offer ? offer->resource : NULL);
}

static void
set_selection (struct wl_client *client, struct wl_resource *resource,
               struct wl_resource *source_resource, uint32_t serial)
{
    struct DataDevice *device = wl_resource_get_user_data (resource);
    struct DataSeat *seat = device->seat;
    struct SeatSelection *selection = &seat->selections[device->kind];
    struct MbDataSource *source =
        source_resource ? wl_resource_get_user_data (source_resource) : NULL;

    if (seat->removed ||
        !mb_input_selection_serial (seat->id, device->client, serial) ||
        (selection->serial_set &&
         (uint32_t)(serial - selection->serial) > INT32_MAX))
        return;
    if (source && source->use != SOURCE_UNUSED)
    {
        if (source == selection->source && serial == selection->serial)
            return;
        if (device->kind == MB_SELECTION_CLIPBOARD)
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
    if (selection->source)
    {
        selection->source->cancelled = TRUE;
        source_notify (selection->source, MB_SOURCE_CANCELLED, NULL, 0);
    }
    invalidate_selection (seat, device->kind);
    selection->source = source;
    selection->serial = serial;
    selection->serial_set = TRUE;
    if (source)
        source->use = SOURCE_SELECTION;
    announce_selection (seat, device->kind);
    if (compositor.data->bridge)
        compositor.data->bridge->selection (compositor.data->bridge_data,
                                            device->kind, seat->id, source);
}

static void
update_action (struct DataOffer *offer)
{
    struct DataDrag *drag = offer->drag;
    uint32_t common, action;

    if (!drag || !offer->active)
        return;
    common = drag->source->actions & offer->actions;
    action = common & offer->preferred;
    if (!action)
    {
        if (common & WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY)
            action = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
        else if (common & WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE)
            action = WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;
        else if (common & WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK)
            action = WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK;
    }
    if (drag->dropped)
    {
        offer->action = offer->preferred;
        return;
    }
    if (!offer->action_sent || offer->action != action)
    {
        offer->action = action;
        offer->action_sent = TRUE;
        if (wl_resource_get_version (offer->resource) >=
            WL_DATA_OFFER_ACTION_SINCE_VERSION)
            wl_data_offer_send_action (offer->resource, action);
    }
    if (!drag->winner || drag->winner == offer)
    {
        if ((!drag->action_sent || drag->source_action != action) &&
            drag->source)
        {
            drag->source_action = action;
            drag->action_sent = TRUE;
            source_notify (drag->source, MB_SOURCE_ACTION, NULL, action);
        }
    }
}

static void
clear_targets (struct DataDrag *drag)
{
    struct DragTarget *target, *next;

    wl_list_for_each_safe (target, next, &drag->targets, link)
    {
        if (target->entered && !target->device->client->failed)
            wl_data_device_send_leave (target->device->resource);
        if (target->offer)
        {
            target->offer->drag = NULL;
            target->offer->target = NULL;
            target->offer->active = FALSE;
        }
        wl_list_remove (&target->link);
        free (target);
    }
    drag->winner = NULL;
    drag->focus = NULL;
}

static void
hide_icon (struct DataDrag *drag)
{
    WindowPtr window =
        drag->icon ? mb_window_get (&drag->icon->window, DixSetAttrAccess)
                   : NULL;

    if (window)
        UnmapWindow (window, FALSE);
}

static void
end_drag (struct DataDrag *drag, Bool success)
{
    struct MbDataSource *source = drag->source;

    if (drag->pointer_active)
    {
        drag->seat->drag = NULL;
        drag->pointer_active = FALSE;
        if (!drag->external)
            mb_input_end_drag (drag->seat->id);
    }
    if (drag->native_target)
        mb_xdnd_cancel (drag->seat->id);
    hide_icon (drag);
    clear_targets (drag);
    if (source)
    {
        if (success)
            source_notify (source, MB_SOURCE_FINISHED, NULL, 0);
        else if (!source->cancelled)
        {
            source->cancelled = TRUE;
            if (source_version (source) >=
                WL_DATA_SOURCE_DND_DROP_PERFORMED_SINCE_VERSION)
                source_notify (source, MB_SOURCE_CANCELLED, NULL, 0);
        }
    }
    wl_list_remove (&drag->link);
    free (drag);
}

static Bool
position_icon (struct DataDrag *drag)
{
    MbSurface *icon = drag->icon;
    WindowPtr window;
    int64_t x, y;
    XID values[3];

    if (!icon)
        return TRUE;
    window = mb_window_get (&icon->window, DixSetAttrAccess);
    if (!window)
        return TRUE;
    x = (int64_t)drag->position.root_x + drag->icon_x + icon->scene_x;
    y = (int64_t)drag->position.root_y + drag->icon_y + icon->scene_y;
    if (!icon->content || drag->position.screen != icon->window.screen ||
        x < INT16_MIN || x > INT16_MAX || y < INT16_MIN || y > INT16_MAX)
    {
        UnmapWindow (window, FALSE);
        return TRUE;
    }
    values[0] = (int32_t)x;
    values[1] = (int32_t)y;
    values[2] = Above;
    return ConfigureWindow (window, CWX | CWY | CWStackMode, values,
                            icon->client->native) == Success &&
           MapWindow (window, icon->client->native) == Success;
}

static Bool
shape_icon (MbSurface *surface)
{
    WindowPtr window = mb_window_get (&surface->window, DixSetAttrAccess);
    PicturePtr picture;
    RegionPtr bounds;
    CARD32 *pixels;
    Bool overlap;
    int row, column;

    if (!window || !surface->content)
        return TRUE;
    picture = mb_scene_picture (surface);
    bounds = RegionCreate (NULL, 0);
    pixels = calloc (surface->scene_width, sizeof (*pixels));
    if (!picture || !bounds || !pixels)
        goto fail;
    for (row = 0; row < surface->scene_height; row++)
    {
        surface->window.screen->GetImage (picture->pDrawable, 0, row,
                                          surface->scene_width, 1, ZPixmap,
                                          ~0UL, (char *)pixels);
        for (column = 0; column < surface->scene_width;)
        {
            int first;
            BoxRec box;
            RegionRec run;
            Bool appended;

            while (column < surface->scene_width &&
                   !(pixels[column] >> ICON_ALPHA_SHIFT))
                column++;
            first = column;
            while (column < surface->scene_width &&
                   pixels[column] >> ICON_ALPHA_SHIFT)
                column++;
            if (first == column)
                continue;
            box = (BoxRec){ first, row, column, row + 1 };
            RegionInit (&run, &box, 0);
            appended = RegionAppend (bounds, &run);
            RegionUninit (&run);
            if (!appended)
                goto fail;
        }
    }
    if (!RegionValidate (bounds, &overlap) || !MakeWindowOptional (window))
        goto fail;
    if (window->optional->boundingShape)
        RegionDestroy (window->optional->boundingShape);
    window->optional->boundingShape = bounds;
    surface->window.screen->SetShape (window, ShapeBounding);
    SendShapeNotify (window, ShapeBounding);
    FreePicture (picture, None);
    free (pixels);
    return TRUE;

fail:
    if (picture)
        FreePicture (picture, None);
    if (bounds)
        RegionDestroy (bounds);
    free (pixels);
    return FALSE;
}

Bool
mb_data_icon_changed (MbSurface *surface)
{
    struct DataDrag *drag;

    if (!compositor.data || surface->role != MB_ROLE_DRAG_ICON)
        return TRUE;
    wl_list_for_each (drag, &compositor.data->drags, link)
    {
        if (drag->icon != surface || !drag->pointer_active)
            continue;
        drag->icon_x += surface->offset_x;
        drag->icon_y += surface->offset_y;
        return shape_icon (surface) && position_icon (drag);
    }
    return TRUE;
}

void
mb_data_drag_motion (uint64_t id, const MbDragPosition *position)
{
    struct DataSeat *seat;
    struct DataDrag *drag = NULL;
    MbSurface *surface = position->surface;
    struct DragTarget *target;
    Bool moved;

    if (!compositor.data)
        return;
    wl_list_for_each (seat, &compositor.data->seats, link)
    {
        if (seat->id == id)
        {
            drag = seat->drag;
            break;
        }
    }
    if (!drag || !drag->pointer_active)
        return;
    if (!drag->external)
    {
        drag->native_target = mb_xdnd_motion (id, drag->source, position);
        if (drag->native_target)
            surface = NULL;
    }
    moved = drag->position.x != position->x || drag->position.y != position->y;
    drag->position = *position;
    if (!position_icon (drag) && drag->icon)
        wl_resource_post_no_memory (drag->icon->resource);
    if (surface &&
        (surface->client->failed ||
         (!drag->source && surface->client->lifetime != drag->origin_client)))
        surface = NULL;
    if (drag->focus != surface)
    {
        struct DataDevice *device;

        clear_targets (drag);
        if (drag->source)
        {
            source_notify (drag->source, MB_SOURCE_TARGET, NULL, 0);
            source_notify (drag->source, MB_SOURCE_ACTION, NULL,
                           WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE);
            drag->source_action = WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
            drag->action_sent = TRUE;
        }
        drag->focus = surface;
        if (!surface)
            return;
        drag->enter_serial = wl_display_next_serial (compositor.wl_display);
        wl_list_for_each (device, &compositor.data->devices, link)
        {
            if (device->kind != MB_SELECTION_CLIPBOARD ||
                device->seat != drag->seat ||
                device->client != surface->client || device->client->failed)
                continue;
            target = calloc (1, sizeof (*target));
            if (!target)
            {
                wl_resource_post_no_memory (device->resource);
                continue;
            }
            target->device = device;
            if (drag->source)
            {
                target->offer = create_offer (device, drag->source, drag);
                if (!target->offer)
                {
                    free (target);
                    continue;
                }
                target->offer->target = target;
            }
            target->entered = TRUE;
            wl_list_insert (drag->targets.prev, &target->link);
            wl_data_device_send_enter (
                device->resource, drag->enter_serial, surface->resource,
                position->x, position->y,
                target->offer ? target->offer->resource : NULL);
            if (target->offer)
                update_action (target->offer);
        }
    }
    else if (moved && surface)
    {
        wl_list_for_each (target, &drag->targets, link)
        {
            if (target->entered && !target->device->client->failed)
                wl_data_device_send_motion (target->device->resource,
                                            position->time, position->x,
                                            position->y);
        }
    }
}

void
mb_data_drag_drop (uint64_t id)
{
    struct DataSeat *seat;
    struct DataDrag *drag = NULL;
    struct DragTarget *target;
    struct DataOffer *winner;

    if (!compositor.data)
        return;
    wl_list_for_each (seat, &compositor.data->seats, link)
    {
        if (seat->id == id)
        {
            drag = seat->drag;
            break;
        }
    }
    if (!drag)
        return;
    if (drag->source)
        source_notify (drag->source, MB_SOURCE_DROP, NULL, 0);
    if (drag->native_target)
    {
        if (!mb_xdnd_drop (id))
        {
            end_drag (drag, FALSE);
            return;
        }
        drag->dropped = TRUE;
        drag->pointer_active = FALSE;
        drag->seat->drag = NULL;
        mb_input_end_drag (id);
        hide_icon (drag);
        drag->icon = NULL;
        return;
    }
    if (!drag->source)
    {
        Bool accepted = !wl_list_empty (&drag->targets);

        wl_list_for_each (target, &drag->targets, link)
        {
            if (target->entered && !target->device->client->failed)
                wl_data_device_send_drop (target->device->resource);
        }
        end_drag (drag, accepted);
        return;
    }
    winner = drag->winner;
    if (!winner)
    {
        wl_list_for_each (target, &drag->targets, link)
        {
            if (target->offer && target->offer->active &&
                wl_resource_get_version (target->offer->resource) <
                    WL_DATA_OFFER_FINISH_SINCE_VERSION)
            {
                winner = target->offer;
                break;
            }
        }
    }
    if (!winner || !winner->active || !winner->action ||
        (!winner->accepted && wl_resource_get_version (winner->resource) >=
                                  WL_DATA_OFFER_FINISH_SINCE_VERSION))
    {
        end_drag (drag, FALSE);
        return;
    }
    drag->winner = winner;
    drag->dropped = TRUE;
    drag->ask = winner->action == WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK;
    drag->pointer_active = FALSE;
    drag->seat->drag = NULL;
    if (!drag->external)
        mb_input_end_drag (id);
    hide_icon (drag);
    drag->icon = NULL;
    wl_list_for_each (target, &drag->targets, link)
    {
        if (target->offer == winner)
            wl_data_device_send_drop (target->device->resource);
        else
        {
            if (target->entered && !target->device->client->failed)
                wl_data_device_send_leave (target->device->resource);
            target->entered = FALSE;
            if (target->offer)
            {
                target->offer->active = FALSE;
                target->offer->drag = NULL;
            }
        }
    }
}

Bool
mb_data_drag_import (uint64_t id, MbDataSource *source)
{
    struct DataSeat *seat;
    struct DataDrag *drag;

    wl_list_for_each (seat, &compositor.data->seats, link)
    {
        if (seat->id != id || seat->removed || seat->drag)
            continue;
        drag = calloc (1, sizeof (*drag));
        if (!drag)
            return FALSE;
        drag->source = source;
        drag->seat = seat;
        drag->external = TRUE;
        drag->pointer_active = TRUE;
        wl_list_init (&drag->targets);
        wl_list_insert (compositor.data->drags.prev, &drag->link);
        source->use = SOURCE_DRAG;
        seat->drag = drag;
        return TRUE;
    }
    return FALSE;
}

void
mb_data_drag_source_actions (uint64_t id, uint32_t actions)
{
    struct DataDrag *drag;
    struct DragTarget *target;

    wl_list_for_each (drag, &compositor.data->drags, link)
    {
        if (drag->seat->id != id || !drag->external || drag->dropped ||
            drag->source->actions == actions)
            continue;
        drag->source->actions = actions;
        wl_list_for_each (target, &drag->targets, link)
        {
            if (!target->offer || !target->offer->active)
                continue;
            if (wl_resource_get_version (target->offer->resource) >=
                WL_DATA_OFFER_SOURCE_ACTIONS_SINCE_VERSION)
                wl_data_offer_send_source_actions (target->offer->resource,
                                                   actions);
            update_action (target->offer);
        }
    }
}

void
mb_data_drag_native_action (uint64_t id, uint32_t action)
{
    struct DataDrag *drag;

    wl_list_for_each (drag, &compositor.data->drags, link)
    {
        if (drag->seat->id == id && drag->native_target && drag->source &&
            drag->source_action != action)
        {
            drag->source_action = action;
            source_notify (drag->source, MB_SOURCE_ACTION, NULL, action);
        }
    }
}

void
mb_data_drag_native_finish (uint64_t id, Bool success)
{
    struct DataDrag *drag, *next;

    wl_list_for_each_safe (drag, next, &compositor.data->drags, link)
    {
        if (drag->seat->id == id && drag->native_target)
            end_drag (drag, success);
    }
}

void
mb_data_drag_cancel (uint64_t id)
{
    struct DataDrag *drag, *next;

    if (!compositor.data)
        return;
    wl_list_for_each_safe (drag, next, &compositor.data->drags, link)
    {
        if (drag->seat->id == id && drag->pointer_active)
            end_drag (drag, FALSE);
    }
}

void
mb_data_surface_destroyed (MbSurface *surface)
{
    struct DataDrag *drag, *next;

    if (!compositor.data)
        return;
    wl_list_for_each_safe (drag, next, &compositor.data->drags, link)
    {
        if (drag->icon == surface)
            drag->icon = NULL;
        if (drag->origin == surface)
        {
            drag->origin = NULL;
            if (drag->pointer_active)
            {
                end_drag (drag, FALSE);
                continue;
            }
        }
        if (drag->focus == surface)
        {
            if (drag->dropped)
                end_drag (drag, FALSE);
            else
                clear_targets (drag);
        }
    }
}

static void
start_drag (struct wl_client *client, struct wl_resource *resource,
            struct wl_resource *source_resource,
            struct wl_resource *origin_resource,
            struct wl_resource *icon_resource, uint32_t serial)
{
    struct DataDevice *device = wl_resource_get_user_data (resource);
    struct MbDataSource *source =
        source_resource ? wl_resource_get_user_data (source_resource) : NULL;
    MbSurface *origin = wl_resource_get_user_data (origin_resource);
    MbSurface *icon =
        icon_resource ? wl_resource_get_user_data (icon_resource) : NULL;
    struct DataDrag *drag, *existing;
    MbDragPosition position;
    XID override = TRUE;
    WindowPtr window;

    if (source && source->use != SOURCE_UNUSED)
    {
        wl_resource_post_error (resource, DATA_DEVICE_USED_SOURCE,
                                "data source has already been used");
        return;
    }
    if (icon && icon->role != MB_ROLE_NONE && icon->role != MB_ROLE_DRAG_ICON)
    {
        wl_resource_post_error (resource, WL_DATA_DEVICE_ERROR_ROLE,
                                "drag icon surface already has a role");
        return;
    }
    wl_list_for_each (existing, &compositor.data->drags, link)
    {
        if (icon && existing->icon == icon && existing->pointer_active)
        {
            wl_resource_post_error (resource, WL_DATA_DEVICE_ERROR_ROLE,
                                    "drag icon is already in use");
            return;
        }
    }
    if (device->seat->removed || device->seat->drag ||
        origin->client != device->client)
        return;
    drag = calloc (1, sizeof (*drag));
    if (!drag)
    {
        wl_resource_post_no_memory (resource);
        return;
    }
    if (!mb_input_begin_drag (device->seat->id, origin, serial, &position))
    {
        free (drag);
        return;
    }
    wl_list_init (&drag->targets);
    drag->seat = device->seat;
    drag->origin_device = device;
    drag->source = source;
    drag->origin = origin;
    drag->origin_client = device->client->lifetime;
    drag->icon = icon;
    drag->pointer_active = TRUE;
    if (source)
    {
        source->use = SOURCE_DRAG;
        if (source_version (source) < WL_DATA_SOURCE_SET_ACTIONS_SINCE_VERSION)
            source->actions = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
    }
    device->seat->drag = drag;
    wl_list_insert (compositor.data->drags.prev, &drag->link);
    if (icon)
    {
        icon->role = MB_ROLE_DRAG_ICON;
        window = mb_window_get (&icon->window, DixSetAttrAccess);
        if (!window ||
            ChangeWindowAttributes (window, CWOverrideRedirect, &override,
                                    icon->client->native) != Success)
        {
            wl_resource_post_no_memory (resource);
            end_drag (drag, FALSE);
            return;
        }
        drag->position = position;
        if (!mb_surface_update (icon))
        {
            end_drag (drag, FALSE);
            return;
        }
    }
    mb_data_drag_motion (device->seat->id, &position);
}

static void
device_destroyed (struct wl_resource *resource)
{
    struct DataDevice *device = wl_resource_get_user_data (resource);
    struct DataDrag *drag, *next;

    wl_list_for_each_safe (drag, next, &compositor.data->drags, link)
    {
        struct DragTarget *target;
        Bool cancel = drag->origin_device == device && drag->pointer_active;

        if (drag->origin_device == device)
            drag->origin_device = NULL;
        wl_list_for_each (target, &drag->targets, link)
        {
            if (target->device == device)
            {
                cancel |= target->entered;
                if (!target->entered)
                    target->device = NULL;
            }
        }
        if (cancel)
            end_drag (drag, FALSE);
    }
    wl_list_remove (&device->link);
    free (device);
}

static const struct wl_data_device_interface device_implementation = {
    .start_drag = start_drag,
    .set_selection = set_selection,
    .release = release,
};

static const struct zwp_primary_selection_device_v1_interface
    primary_device = {
        .set_selection = set_selection,
        .destroy = release,
    };

static void
get_device (struct wl_client *client, struct wl_resource *resource,
            uint32_t id, struct wl_resource *seat_resource)
{
    uint64_t seat_id = mb_input_seat_id (seat_resource);
    Bool primary = wl_resource_get_user_data (resource) != NULL;
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
        for (MbSelectionKind kind = 0; kind < MB_SELECTION_COUNT; kind++)
            seat->selections[kind].source = compositor.data->imported[kind];
        wl_list_insert (compositor.data->seats.prev, &seat->link);
    }
    device = calloc (1, sizeof (*device));
    if (!device)
        goto fail;
    device->resource = wl_resource_create (
        client,
        primary ? &zwp_primary_selection_device_v1_interface
                : &wl_data_device_interface,
        wl_resource_get_version (resource), id);
    if (!device->resource)
    {
        free (device);
        goto fail;
    }
    device->seat = seat;
    device->kind = primary ? MB_SELECTION_PRIMARY : MB_SELECTION_CLIPBOARD;
    device->client = mb_client_for_wayland (client);
    wl_list_insert (compositor.data->devices.prev, &device->link);
    wl_resource_set_implementation (device->resource,
                                    primary ? (const void *)&primary_device
                                            : &device_implementation,
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

static const struct zwp_primary_selection_device_manager_v1_interface
    primary_manager = {
        .create_source = create_source,
        .get_device = get_device,
        .destroy = release,
    };

static void
bind_manager (struct wl_client *client, void *data, uint32_t version,
              uint32_t id)
{
    const struct wl_interface *interface =
        data ? &zwp_primary_selection_device_manager_v1_interface
             : &wl_data_device_manager_interface;
    struct wl_resource *resource =
        wl_resource_create (client, interface, version, id);

    if (!resource)
    {
        wl_client_post_no_memory (client);
        return;
    }
    wl_resource_set_implementation (
        resource, data ? (const void *)&primary_manager : &implementation,
        data, NULL);
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
        struct DataDevice *device;

        wl_list_for_each (device, &compositor.data->devices, link)
        {
            if (device->seat == seat && device->kind == MB_SELECTION_PRIMARY &&
                device->client->lifetime == seat->focus &&
                !device->client->failed)
                zwp_primary_selection_device_v1_send_selection (
                    device->resource, NULL);
        }
        seat->focus = focus;
        for (MbSelectionKind kind = 0; kind < MB_SELECTION_COUNT; kind++)
        {
            invalidate_selection (seat, kind);
            announce_selection (seat, kind);
        }
    }
}

void
mb_data_seat_removed (uint64_t id)
{
    struct DataSeat *seat;
    struct DataDrag *drag, *next;

    if (!compositor.data)
        return;
    wl_list_for_each_safe (drag, next, &compositor.data->drags, link)
    {
        if (drag->seat->id == id)
            end_drag (drag, FALSE);
    }
    wl_list_for_each (seat, &compositor.data->seats, link)
    {
        if (seat->id != id)
            continue;
        for (MbSelectionKind kind = 0; kind < MB_SELECTION_COUNT; kind++)
        {
            MbDataSource *source = seat->selections[kind].source;

            if (source && source != compositor.data->imported[kind])
            {
                source->cancelled = TRUE;
                source_notify (source, MB_SOURCE_CANCELLED, NULL, 0);
            }
            seat->selections[kind].source = NULL;
            invalidate_selection (seat, kind);
        }
        if (compositor.data->bridge && compositor.data->bridge->removed)
            compositor.data->bridge->removed (compositor.data->bridge_data,
                                              seat->id);
        seat->focus = 0;
        seat->removed = TRUE;
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
    wl_list_init (&data->drags);
    compositor.data = data;
    data->globals[MB_SELECTION_CLIPBOARD] = wl_global_create (
        compositor.wl_display, &wl_data_device_manager_interface, DATA_VERSION,
        NULL, bind_manager);
    data->globals[MB_SELECTION_PRIMARY] = wl_global_create (
        compositor.wl_display,
        &zwp_primary_selection_device_manager_v1_interface,
        zwp_primary_selection_device_manager_v1_interface.version, data,
        bind_manager);
    if (!data->globals[MB_SELECTION_CLIPBOARD] ||
        !data->globals[MB_SELECTION_PRIMARY])
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
    struct MbDataSource *source, *next_source;
    struct DataOffer *offer, *next_offer;
    struct DataSeat *seat, *next_seat;
    struct DataDrag *drag, *next_drag;

    if (!data)
        return;
    for (MbSelectionKind kind = 0; kind < MB_SELECTION_COUNT; kind++)
        if (data->globals[kind])
            wl_global_destroy (data->globals[kind]);
    wl_list_for_each_safe (drag, next_drag, &data->drags, link)
    {
        end_drag (drag, FALSE);
    }
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
        mb_data_source_destroy (source);
    }
    wl_list_for_each_safe (seat, next_seat, &data->seats, link)
    {
        wl_list_remove (&seat->link);
        free (seat);
    }
    free (data);
    compositor.data = NULL;
}
