/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "server.h"

#include <X11/Xatom.h>
#include <dixevents.h>
#include <errno.h>
#include <fcntl.h>
#include <propertyst.h>
#include <selection.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <window.h>
#include <xace.h>

#define SEND_EVENT_MASK 0x80
#define TRANSFER_CHUNK (64 * 1024)
#define TRANSFER_LIMIT 32
#define TRANSFER_TIMEOUT_MS 10000
#define TARGET_LIMIT 256
#define MIME_UTF8 "text/plain;charset=utf-8"
#define MIME_TEXT "text/plain"
#define SELECTION_EVENT_MASK (PropertyChangeMask | StructureNotifyMask)

typedef struct
{
    Atom clipboard, targets, timestamp, incr, text, multiple, atom_pair;
    Atom transfer, xdnd;
} SelectionAtoms;

typedef struct
{
    Atom target;
    const char *mime;
} SelectionType;

typedef struct
{
    struct wl_list link;
    MbDataSource *source;
    MbSelectionNotify notify;
    void *data;
    Window owner;
    Atom selection;
    TimeStamp time;
    SelectionType types[TARGET_LIMIT];
    size_t count;
} Import;

typedef struct Transfer Transfer;
struct Transfer
{
    struct wl_list link;
    struct wl_event_source *io, *timer;
    Window window, owner;
    Atom selection, target, property, type;
    Time time;
    int fd;
    Bool incoming, incr, notified, waiting, eof, targets;
    unsigned char bytes[TRANSFER_CHUNK];
    size_t length, offset;
    uint32_t property_size, property_offset, format;
    CARD32 atoms[TARGET_LIMIT];
    size_t atom_count;
    TimeStamp epoch;
};

typedef struct
{
    struct wl_list link;
    Window window;
} ForeignWatch;

struct MbSelections
{
    SelectionAtoms atoms;
    struct wl_list transfers, imports, watches;
    Window window;
    MbDataSource *exported, *imported, *drag;
    uint64_t seat;
    TimeStamp ownership, drag_time;
    Bool dirty, registered, stopping;
};

static void transfer_destroy (Transfer *transfer);
static void import_targets (Transfer *transfer);
static Bool incoming_pump (Transfer *transfer);
static int incoming_ready (int fd, uint32_t mask, void *data);
static int outgoing_ready (int fd, uint32_t mask, void *data);

static Bool
transfer_watch (Transfer *transfer, uint32_t mask)
{
    if (!mask)
    {
        if (transfer->io)
            wl_event_source_remove (transfer->io);
        transfer->io = NULL;
        return TRUE;
    }
    if (transfer->io)
        return wl_event_source_fd_update (transfer->io, mask) == 0;
    transfer->io = wl_event_loop_add_fd (
        compositor.wl_event_loop, transfer->fd, mask,
        transfer->incoming ? incoming_ready : outgoing_ready, transfer);
    return transfer->io != NULL;
}

static ClientPtr
bridge (void)
{
    return compositor.bridge_client->native;
}

static WindowPtr
lookup_window (Window id, Mask access)
{
    WindowPtr window;

    if (!id || !bridge () ||
        dixLookupWindow (&window, id, bridge (), access) != Success)
        return NULL;
    return window;
}

static Window
create_window (void)
{
    XID id = FakeClientID (bridge ()->index);
    XID mask = SELECTION_EVENT_MASK;
    WindowPtr window;
    int error;

    window = CreateWindow (id, screenInfo.screens[0]->root, 0, 0,
                           MB_INITIAL_WINDOW_SIZE, MB_INITIAL_WINDOW_SIZE, 0,
                           InputOnly, CWEventMask, &mask, 0, bridge (),
                           CopyFromParent, &error);
    if (!window)
        return None;
    /* AddResource may destroy the window on failure. */
    window->eventMask = NoEventMask;
    if (!AddResource (id, RT_WINDOW, window))
        return None;
    window->eventMask = SELECTION_EVENT_MASK;
    return id;
}

static Bool
property_write (Window id, Atom property, Atom type, int format, size_t count,
                const void *bytes)
{
    WindowPtr window = lookup_window (id, DixSetPropAccess);

    return window && dixChangeWindowProperty (bridge (), window, property,
                                              type, format, PropModeReplace,
                                              count, bytes, TRUE) == Success;
}

static void
property_delete (Window id, Atom property)
{
    WindowPtr window = lookup_window (id, DixSetPropAccess);

    if (window)
        DeleteProperty (bridge (), window, property);
}

static void
selection_notify (Transfer *transfer, Atom property)
{
    WindowPtr window = lookup_window (transfer->window, DixSendAccess);
    xEvent event = { 0 };

    if (!window)
        return;
    event.u.u.type = SelectionNotify;
    event.u.selectionNotify.requestor = transfer->window;
    event.u.selectionNotify.selection = transfer->selection;
    event.u.selectionNotify.target = transfer->target;
    event.u.selectionNotify.property = property;
    event.u.selectionNotify.time = transfer->time;
    if (XaceHookSendAccess (bridge (), NULL, window, &event, 1) == Success)
        WriteEventsToClient (wClient (window), 1, &event);
}

static Bool
refresh_watch (Window id)
{
    Transfer *transfer;
    ForeignWatch *watch;
    Mask mask = NoEventMask;
    WindowPtr window = lookup_window (id, DixReceiveAccess);

    if (!window)
        return FALSE;
    wl_list_for_each (watch, &compositor.selections->watches, link)
    {
        if (watch->window == id)
            mask = SELECTION_EVENT_MASK;
    }
    wl_list_for_each (transfer, &compositor.selections->transfers, link)
    {
        if ((transfer->incoming ? transfer->owner : transfer->window) == id)
            mask = SELECTION_EVENT_MASK;
    }
    return EventSelectForWindow (window, bridge (), mask) == Success;
}

Bool
mb_selection_watch (Window id, Bool enable)
{
    ForeignWatch *watch;

    wl_list_for_each (watch, &compositor.selections->watches, link)
    {
        if (watch->window == id)
            break;
    }
    if (&watch->link == &compositor.selections->watches)
    {
        if (!enable)
            return TRUE;
        watch = calloc (1, sizeof (*watch));
        if (!watch)
            return FALSE;
        watch->window = id;
        wl_list_insert (&compositor.selections->watches, &watch->link);
    }
    if (!enable)
    {
        wl_list_remove (&watch->link);
        free (watch);
    }
    return refresh_watch (id);
}

static void
transfer_destroy (Transfer *transfer)
{
    wl_list_remove (&transfer->link);
    if (transfer->io)
        wl_event_source_remove (transfer->io);
    if (transfer->timer)
        wl_event_source_remove (transfer->timer);
    if (transfer->fd >= 0)
        close (transfer->fd);
    if (transfer->incoming)
    {
        if (lookup_window (transfer->window, DixDestroyAccess))
            FreeResource (transfer->window, RT_NONE);
    }
    else if (!transfer->notified)
        selection_notify (transfer, None);
    refresh_watch (transfer->incoming ? transfer->owner : transfer->window);
    free (transfer);
}

static int
transfer_timeout (void *data)
{
    transfer_destroy (data);
    return 0;
}

static Transfer *
transfer_create (Bool incoming)
{
    struct MbSelections *selections = compositor.selections;
    Transfer *transfer;

    if (wl_list_length (&selections->transfers) >= TRANSFER_LIMIT)
        return NULL;
    transfer = calloc (1, sizeof (*transfer));
    if (!transfer)
        return NULL;
    transfer->fd = -1;
    transfer->incoming = incoming;
    wl_list_insert (selections->transfers.prev, &transfer->link);
    transfer->timer = wl_event_loop_add_timer (compositor.wl_event_loop,
                                               transfer_timeout, transfer);
    if (!transfer->timer)
    {
        transfer_destroy (transfer);
        return NULL;
    }
    wl_event_source_timer_update (transfer->timer, TRANSFER_TIMEOUT_MS);
    return transfer;
}

static void
selection_convert (Transfer *transfer)
{
    Selection *selection;
    xEvent event = { 0 };
    SelectionBridgeInfoRec info;

    if (dixLookupSelection (&selection, transfer->selection, bridge (),
                            DixReadAccess) != Success ||
        selection->window != transfer->owner)
        goto fail;
    event.u.u.type = SelectionRequest;
    event.u.selectionRequest.owner = selection->window;
    event.u.selectionRequest.requestor = transfer->window;
    event.u.selectionRequest.selection = transfer->selection;
    event.u.selectionRequest.target = transfer->target;
    event.u.selectionRequest.property = transfer->property;
    event.u.selectionRequest.time = transfer->time;
    if (selection->client && selection->client != serverClient &&
        !selection->client->clientGone)
    {
        WriteEventsToClient (selection->client, 1, &event);
        return;
    }
    if (selection->client == serverClient && SelectionBridgeCallback)
    {
        info = (SelectionBridgeInfoRec){
            .screen = selection->pWin->drawable.pScreen,
            .client = bridge (),
            .requestor = transfer->window,
            .selection = transfer->selection,
            .target = transfer->target,
            .property = transfer->property,
            .time = ClientTimeToServerTime (transfer->time),
        };
        CallCallbacks (&SelectionBridgeCallback, &info);
        return;
    }
fail:
    transfer_destroy (transfer);
}

static Bool
outgoing_pump (Transfer *transfer)
{
    struct MbSelections *selections = compositor.selections;
    ssize_t count;

    while (!transfer->eof && transfer->length < sizeof (transfer->bytes))
    {
        count = read (transfer->fd, transfer->bytes + transfer->length,
                      sizeof (transfer->bytes) - transfer->length);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && errno == EAGAIN)
            break;
        if (count < 0)
            goto fail;
        if (!count)
            transfer->eof = TRUE;
        else
        {
            transfer->length += count;
            wl_event_source_timer_update (transfer->timer,
                                          TRANSFER_TIMEOUT_MS);
        }
    }
    if (!transfer->notified &&
        (transfer->eof || transfer->length == sizeof (transfer->bytes)))
    {
        if (transfer->eof)
        {
            if (!property_write (transfer->window, transfer->property,
                                 transfer->type, MB_PROPERTY_FORMAT_8,
                                 transfer->length, transfer->bytes))
                goto fail;
            selection_notify (transfer, transfer->property);
            transfer->notified = TRUE;
            transfer_destroy (transfer);
            return FALSE;
        }
        CARD32 lower_bound = transfer->length;

        if (!property_write (transfer->window, transfer->property,
                             selections->atoms.incr, MB_PROPERTY_FORMAT_32, 1,
                             &lower_bound))
            goto fail;
        selection_notify (transfer, transfer->property);
        transfer->notified = TRUE;
        transfer->incr = TRUE;
        transfer->waiting = TRUE;
    }
    if (transfer->incr && !transfer->waiting &&
        (transfer->eof || transfer->length == sizeof (transfer->bytes)))
    {
        if (!property_write (transfer->window, transfer->property,
                             transfer->type, MB_PROPERTY_FORMAT_8,
                             transfer->length, transfer->bytes))
            goto fail;
        if (!transfer->length)
        {
            transfer_destroy (transfer);
            return FALSE;
        }
        transfer->length = 0;
        transfer->waiting = TRUE;
    }
    if (!transfer_watch (transfer,
                         transfer->eof ||
                                 transfer->length == sizeof (transfer->bytes)
                             ? 0
                             : WL_EVENT_READABLE))
        goto fail;
    return TRUE;
fail:
    transfer_destroy (transfer);
    return FALSE;
}

static int
outgoing_ready (int fd, uint32_t mask, void *data)
{
    Transfer *transfer = data;

    if (mask & WL_EVENT_ERROR)
        transfer_destroy (transfer);
    else
        outgoing_pump (transfer);
    return 0;
}

static Bool
incoming_pump (Transfer *transfer)
{
    WindowPtr window;
    PropertyPtr property;
    size_t count, available;
    ssize_t written;

    for (;;)
    {
        while (transfer->offset < transfer->length)
        {
            written = write (transfer->fd, transfer->bytes + transfer->offset,
                             transfer->length - transfer->offset);
            if (written < 0 && errno == EINTR)
                continue;
            if (written < 0 && errno == EAGAIN)
            {
                if (!transfer_watch (transfer, WL_EVENT_WRITABLE))
                    goto fail;
                return TRUE;
            }
            if (written <= 0)
                goto fail;
            transfer->offset += written;
            wl_event_source_timer_update (transfer->timer,
                                          TRANSFER_TIMEOUT_MS);
        }
        transfer->length = transfer->offset = 0;
        available = transfer->property_size - transfer->property_offset;
        if (!available)
            break;
        window = lookup_window (transfer->window, DixGetPropAccess);
        if (!window ||
            dixLookupProperty (&property, window, transfer->property,
                               bridge (), DixReadAccess) != Success ||
            !property || property->type != transfer->type ||
            property->format != transfer->format ||
            property->size != transfer->property_size)
            goto fail;
        if (transfer->targets)
        {
            if (available > TARGET_LIMIT - transfer->atom_count)
                goto fail;
            memcpy (transfer->atoms + transfer->atom_count,
                    (CARD32 *)property->data + transfer->property_offset,
                    available * sizeof (CARD32));
            transfer->atom_count += available;
            transfer->property_offset += available;
            continue;
        }
        count = min (available, sizeof (transfer->bytes) / 2);
        const unsigned char *bytes = property->data;

        bytes += transfer->property_offset;
        if (transfer->type == XA_STRING)
        {
            /* STRING is ISO-8859-1; native text offers are UTF-8. */
            for (size_t i = 0; i < count; i++)
            {
                if (bytes[i] < 0x80)
                    transfer->bytes[transfer->length++] = bytes[i];
                else
                {
                    transfer->bytes[transfer->length++] =
                        0xc0 | (bytes[i] >> 6);
                    transfer->bytes[transfer->length++] =
                        0x80 | (bytes[i] & 0x3f);
                }
            }
        }
        else
        {
            memcpy (transfer->bytes, bytes, count);
            transfer->length = count;
        }
        transfer->property_offset += count;
    }
    transfer_watch (transfer, 0);
    property_delete (transfer->window, transfer->property);
    if (!transfer->incr || !transfer->property_size)
    {
        if (transfer->targets)
            import_targets (transfer);
        transfer_destroy (transfer);
        return FALSE;
    }
    transfer->waiting = TRUE;
    return TRUE;
fail:
    transfer_destroy (transfer);
    return FALSE;
}

static int
incoming_ready (int fd, uint32_t mask, void *data)
{
    if (mask & (WL_EVENT_ERROR | WL_EVENT_HANGUP))
        transfer_destroy (data);
    else
        incoming_pump (data);
    return 0;
}

static void
incoming_property (Transfer *transfer, Bool initial)
{
    WindowPtr window = lookup_window (transfer->window, DixGetPropAccess);
    PropertyPtr property;

    if (!window ||
        dixLookupProperty (&property, window, transfer->property, bridge (),
                           DixReadAccess) != Success ||
        !property)
        goto fail;
    if (initial && property->type == compositor.selections->atoms.incr)
    {
        /* Some owners, including xclip, omit the advisory byte count. */
        if (property->format != MB_PROPERTY_FORMAT_32 || property->size > 1)
            goto fail;
        transfer->incr = TRUE;
        transfer->waiting = TRUE;
        property_delete (transfer->window, transfer->property);
        return;
    }
    if (transfer->targets
            ? property->type != XA_ATOM ||
                  property->format != MB_PROPERTY_FORMAT_32
            : property->format != MB_PROPERTY_FORMAT_8 ||
                  (property->type != transfer->target &&
                   !(transfer->target == compositor.selections->atoms.text &&
                     (property->type == compositor.atoms.utf8_string ||
                      property->type == XA_STRING))))
        goto fail;
    transfer->waiting = FALSE;
    transfer->type = property->type;
    transfer->format = property->format;
    transfer->property_size = property->size;
    transfer->property_offset = 0;
    wl_event_source_timer_update (transfer->timer, TRANSFER_TIMEOUT_MS);
    incoming_pump (transfer);
    return;
fail:
    transfer_destroy (transfer);
}

static Transfer *
incoming_create (Window owner, Atom selection, Atom target, TimeStamp time,
                 int fd)
{
    Transfer *transfer = transfer_create (TRUE);
    int flags;

    if (!transfer)
    {
        if (fd >= 0)
            close (fd);
        return NULL;
    }
    transfer->fd = fd;
    transfer->owner = owner;
    transfer->selection = selection;
    transfer->target = target;
    transfer->epoch = time;
    transfer->time = time.milliseconds;
    transfer->property = compositor.selections->atoms.transfer;
    transfer->window = create_window ();
    WindowPtr owner_window = lookup_window (owner, DixReceiveAccess);

    if (!transfer->window || !owner_window ||
        EventSelectForWindow (owner_window, bridge (), SELECTION_EVENT_MASK) !=
            Success)
        goto fail;
    if (fd >= 0)
    {
        flags = fcntl (fd, F_GETFL);
        if (flags < 0 || fcntl (fd, F_SETFL, flags | O_NONBLOCK) < 0)
            goto fail;
    }
    return transfer;
fail:
    transfer_destroy (transfer);
    return NULL;
}

static void
import_receive (void *data, const char *mime, int fd)
{
    Import *import = data;
    Transfer *transfer;

    for (size_t i = 0; i < import->count; i++)
        if (strcmp (mime, import->types[i].mime) == 0)
        {
            transfer =
                incoming_create (import->owner, import->selection,
                                 import->types[i].target, import->time, fd);
            if (transfer)
                selection_convert (transfer);
            return;
        }
    close (fd);
}

static void
import_notify (void *data, MbSourceEvent event, const char *mime,
               uint32_t action)
{
    Import *import = data;

    if (import->notify)
        import->notify (import->data, event, mime, action);
    if (event != MB_SOURCE_DESTROYED)
        return;
    wl_list_remove (&import->link);
    for (size_t i = 0; i < import->count; i++)
        free ((char *)import->types[i].mime);
    free (import);
}

static const MbDataSourceOps import_ops = {
    .receive = import_receive,
    .notify = import_notify,
};

MbDataSource *
mb_selection_import (Window owner, Atom selection, Time time,
                     const CARD32 *targets, size_t count, uint32_t actions,
                     MbSelectionNotify notify, void *data)
{
    const char *mimes[TARGET_LIMIT];
    Import *import;
    Atom text = None;
    MbDataSource *source;

    if (count > TARGET_LIMIT)
        return NULL;
    import = calloc (1, sizeof (*import));
    if (!import)
        return NULL;
    wl_list_insert (&compositor.selections->imports, &import->link);
    import->owner = owner;
    import->selection = selection;
    import->time = ClientTimeToServerTime (time);
    for (size_t i = 0; i < count; i++)
    {
        Atom atom = targets[i];
        const char *name = NameForAtom (atom);

        if (atom == compositor.atoms.utf8_string)
            text = atom;
        else if (!text && atom == XA_STRING)
            text = atom;
        if (!name || !strchr (name, '/') || import->count >= TARGET_LIMIT - 2)
            continue;
        Bool duplicate = FALSE;
        for (size_t j = 0; j < import->count; j++)
            if (!strcmp (import->types[j].mime, name))
                duplicate = TRUE;
        if (duplicate)
            continue;
        char *mime = strdup (name);
        if (!mime)
            goto fail;
        import->types[import->count++] = (SelectionType){ atom, mime };
    }
    if (text)
        for (size_t i = 0; i < 2; i++)
        {
            const char *mime = i ? MIME_TEXT : MIME_UTF8;
            Bool duplicate = FALSE;

            for (size_t j = 0; j < import->count; j++)
                if (!strcmp (import->types[j].mime, mime))
                    duplicate = TRUE;
            if (!duplicate)
            {
                char *copy = strdup (mime);
                if (!copy)
                    goto fail;
                import->types[import->count++] = (SelectionType){ text, copy };
            }
        }
    if (!import->count)
        goto fail;
    for (size_t i = 0; i < import->count; i++)
        mimes[i] = import->types[i].mime;
    source = mb_data_source_create (mimes, import->count, actions, &import_ops,
                                    import);
    if (!source)
        goto fail;
    import->source = source;
    import->notify = notify;
    import->data = data;
    return source;
fail:
    import_notify (import, MB_SOURCE_DESTROYED, NULL, 0);
    return NULL;
}

void
mb_selection_time (MbDataSource *source, Time time)
{
    Import *import;

    wl_list_for_each (import, &compositor.selections->imports, link)
    {
        if (import->source == source)
            import->time = ClientTimeToServerTime (time);
    }
}

static void
import_targets (Transfer *transfer)
{
    struct MbSelections *selections = compositor.selections;
    Selection *selection;
    MbDataSource *source;

    if (dixLookupSelection (&selection, transfer->selection, bridge (),
                            DixReadAccess) != Success ||
        selection->window != transfer->owner ||
        CompareTimeStamps (selection->lastTimeChanged, transfer->epoch) !=
            SAMETIME)
        return;
    source = mb_selection_import (transfer->owner, transfer->selection,
                                  transfer->time, transfer->atoms,
                                  transfer->atom_count, 0, NULL, NULL);
    if (source)
    {
        selections->imported = source;
        mb_data_import_selection (source);
    }
}

Window
mb_selection_window (void)
{
    return compositor.selections->window;
}

Bool
mb_selection_claim_drag (MbDataSource *source)
{
    struct MbSelections *selections = compositor.selections;
    Selection *selection;

    selections->drag = source;

    if (!bridge ())
        return FALSE;
    if (!source && (dixLookupSelection (&selection, selections->atoms.xdnd,
                                        bridge (), DixReadAccess) != Success ||
                    selection->window != selections->window))
        return TRUE;
    UpdateCurrentTime ();
    selections->drag_time = currentTime;
    return dixSetSelectionOwner (bridge (), selections->atoms.xdnd,
                                 source ? selections->window : None,
                                 currentTime) == Success;
}

static void
selection_request (const xEvent *event)
{
    struct MbSelections *selections = compositor.selections;
    Transfer request = {
        .window = event->u.selectionRequest.requestor,
        .selection = event->u.selectionRequest.selection,
        .target = event->u.selectionRequest.target,
        .property = event->u.selectionRequest.property,
        .time = event->u.selectionRequest.time,
    };
    Transfer *transfer, *other;
    WindowPtr window;
    const char *mime = NULL;
    const char *const *mimes;
    size_t count;
    Atom type = request.target;
    MbDataSource *source =
        request.selection == selections->atoms.clipboard ? selections->exported
        : request.selection == selections->atoms.xdnd    ? selections->drag
                                                         : NULL;
    TimeStamp ownership = request.selection == selections->atoms.clipboard
                              ? selections->ownership
                              : selections->drag_time;
    int pipes[2];

    if (!request.property)
        request.property = request.target;
    if (!source || event->u.selectionRequest.owner != selections->window ||
        (request.time != CurrentTime &&
         CompareTimeStamps (ClientTimeToServerTime (request.time),
                            ownership) == EARLIER))
        goto fail;
    window = lookup_window (request.window, DixSetPropAccess);
    if (!window)
        goto fail;
    mimes = mb_data_source_mimes (source, &count);
    if (request.target == selections->atoms.targets)
    {
        CARD32 targets[TARGET_LIMIT + 3];
        size_t length = 0;
        Bool text = FALSE;

        targets[length++] = selections->atoms.targets;
        targets[length++] = selections->atoms.timestamp;
        for (size_t i = 0; i < count && length < TARGET_LIMIT; i++)
        {
            targets[length++] = MakeAtom (mimes[i], strlen (mimes[i]), TRUE);
            if (!strcmp (mimes[i], MIME_UTF8) || !strcmp (mimes[i], MIME_TEXT))
                text = TRUE;
        }
        if (text)
        {
            targets[length++] = compositor.atoms.utf8_string;
            targets[length++] = selections->atoms.text;
        }
        if (!property_write (request.window, request.property, XA_ATOM,
                             MB_PROPERTY_FORMAT_32, length, targets))
            goto fail;
        selection_notify (&request, request.property);
        return;
    }
    if (request.target == selections->atoms.timestamp)
    {
        CARD32 time = ownership.milliseconds;

        if (!property_write (request.window, request.property, XA_INTEGER,
                             MB_PROPERTY_FORMAT_32, 1, &time))
            goto fail;
        selection_notify (&request, request.property);
        return;
    }
    for (size_t i = 0; i < count; i++)
    {
        if (request.target == MakeAtom (mimes[i], strlen (mimes[i]), FALSE))
        {
            mime = mimes[i];
            break;
        }
        if ((request.target == compositor.atoms.utf8_string ||
             request.target == selections->atoms.text) &&
            (!strcmp (mimes[i], MIME_UTF8) || !strcmp (mimes[i], MIME_TEXT)))
        {
            mime = mimes[i];
            type = compositor.atoms.utf8_string;
        }
    }
    if (!mime)
        goto fail;
    wl_list_for_each (other, &selections->transfers, link)
    {
        if (!other->incoming && other->window == request.window &&
            other->property == request.property)
            goto fail;
    }
    transfer = transfer_create (FALSE);
    if (!transfer)
        goto fail;
    transfer->window = request.window;
    transfer->selection = request.selection;
    transfer->target = request.target;
    transfer->property = request.property;
    transfer->time = request.time;
    transfer->type = type;
    if (EventSelectForWindow (window, bridge (), SELECTION_EVENT_MASK) !=
            Success ||
        pipe2 (pipes, O_CLOEXEC) < 0)
        goto transfer_fail;
    transfer->fd = pipes[0];
    if (fcntl (pipes[0], F_SETFL, O_NONBLOCK) < 0)
    {
        close (pipes[1]);
        goto transfer_fail;
    }
    transfer->io =
        wl_event_loop_add_fd (compositor.wl_event_loop, transfer->fd,
                              WL_EVENT_READABLE, outgoing_ready, transfer);
    if (!transfer->io)
    {
        close (pipes[1]);
        goto transfer_fail;
    }
    if (!mb_data_source_send (source, mime, pipes[1]))
        goto transfer_fail;
    return;
transfer_fail:
    transfer_destroy (transfer);
    return;
fail:
    selection_notify (&request, None);
}

static void
bridge_output (MbClient *client, const void *message, size_t size)
{
    const xEvent *event = message;
    Transfer *transfer, *next;
    int type;

    if (size != sizeof (*event) || !compositor.selections)
        return;
    type = event->u.u.type & ~SEND_EVENT_MASK;
    if (type == SelectionRequest)
    {
        selection_request (event);
        return;
    }
    wl_list_for_each_safe (transfer, next, &compositor.selections->transfers,
                           link)
    {
        if (type == DestroyNotify && !(event->u.u.type & SEND_EVENT_MASK) &&
            !lookup_window (event->u.destroyNotify.window, DixGetAttrAccess) &&
            (event->u.destroyNotify.window == transfer->window ||
             event->u.destroyNotify.window == transfer->owner))
            transfer_destroy (transfer);
        else if (transfer->incoming && type == SelectionNotify &&
                 event->u.selectionNotify.requestor == transfer->window &&
                 event->u.selectionNotify.selection == transfer->selection &&
                 event->u.selectionNotify.target == transfer->target &&
                 event->u.selectionNotify.time == transfer->time)
        {
            if (event->u.selectionNotify.property != transfer->property)
                transfer_destroy (transfer);
            else if (!transfer->notified)
            {
                transfer->notified = TRUE;
                incoming_property (transfer, TRUE);
            }
        }
        else if (type == PropertyNotify &&
                 event->u.property.window == transfer->window &&
                 event->u.property.atom == transfer->property)
        {
            if (transfer->incoming && transfer->notified && transfer->incr &&
                transfer->waiting &&
                event->u.property.state == PropertyNewValue)
                incoming_property (transfer, FALSE);
            else if (!transfer->incoming && transfer->incr &&
                     transfer->waiting &&
                     event->u.property.state == PropertyDelete)
            {
                transfer->waiting = FALSE;
                wl_event_source_timer_update (transfer->timer,
                                              TRANSFER_TIMEOUT_MS);
                outgoing_pump (transfer);
            }
        }
    }
}

static void
selection_changed (CallbackListPtr *list, void *data, void *value)
{
    struct MbSelections *selections = data;
    SelectionInfoRec *info = value;

    if (info->selection->selection == selections->atoms.clipboard)
        selections->dirty = TRUE;
}

static void
export_selection (void *data, uint64_t seat, MbDataSource *source)
{
    struct MbSelections *selections = data;
    MbDataSource *imported;

    if (selections->stopping || (source && source == selections->imported) ||
        (!source && selections->exported && seat != selections->seat))
        return;
    imported = selections->imported;
    selections->imported = NULL;
    selections->exported = source;
    selections->seat = seat;
    if (imported)
        mb_data_source_destroy (imported);
    UpdateCurrentTime ();
    selections->ownership = currentTime;
    if (dixSetSelectionOwner (bridge (), selections->atoms.clipboard,
                              source ? selections->window : None,
                              currentTime) != Success)
        selections->exported = NULL;
}

static void
source_destroyed (void *data, MbDataSource *source)
{
    struct MbSelections *selections = data;

    if (source == selections->imported)
        selections->imported = NULL;
    if (source == selections->exported)
    {
        Selection *selection;

        if (bridge () &&
            dixLookupSelection (&selection, selections->atoms.clipboard,
                                bridge (), DixReadAccess) == Success &&
            selection->window == selections->window)
            export_selection (data, selections->seat, NULL);
        else
            selections->exported = NULL;
    }
    if (source == selections->drag)
        mb_selection_claim_drag (NULL);
}

static void
seat_removed (void *data, uint64_t seat)
{
    struct MbSelections *selections = data;

    if (selections->exported && selections->seat == seat)
        export_selection (data, seat, NULL);
}

static const MbDataBridge data_bridge = {
    .selection = export_selection,
    .destroyed = source_destroyed,
    .removed = seat_removed,
};

void
mb_selections_dispatch (void)
{
    struct MbSelections *selections = compositor.selections;
    Selection *selection;
    Transfer *transfer, *next;

    if (!selections || !selections->dirty)
        return;
    selections->dirty = FALSE;
    if (dixLookupSelection (&selection, selections->atoms.clipboard, bridge (),
                            DixReadAccess) != Success)
        return;
    if (selection->window == selections->window)
        return;
    selections->exported = NULL;
    selections->imported = NULL;
    mb_data_import_selection (NULL);
    wl_list_for_each_safe (transfer, next, &selections->transfers, link)
    {
        if (transfer->incoming && transfer->targets)
            transfer_destroy (transfer);
    }
    if (!selection->window)
        return;
    transfer = incoming_create (selection->window, selection->selection,
                                selections->atoms.targets,
                                selection->lastTimeChanged, -1);
    if (transfer)
    {
        transfer->targets = TRUE;
        selection_convert (transfer);
    }
}

Bool
mb_selections_initialize (void)
{
    struct MbSelections *selections = calloc (1, sizeof (*selections));

    if (!selections)
        return FALSE;
    compositor.selections = selections;
    wl_list_init (&selections->transfers);
    wl_list_init (&selections->imports);
    wl_list_init (&selections->watches);
    struct
    {
        const char *name;
        Atom *atom;
    } atoms[] = {
        { "CLIPBOARD", &selections->atoms.clipboard },
        { "XdndSelection", &selections->atoms.xdnd },
        { "TARGETS", &selections->atoms.targets },
        { "TIMESTAMP", &selections->atoms.timestamp },
        { "INCR", &selections->atoms.incr },
        { "TEXT", &selections->atoms.text },
        { "MULTIPLE", &selections->atoms.multiple },
        { "ATOM_PAIR", &selections->atoms.atom_pair },
        { "_MEATBALL_SELECTION", &selections->atoms.transfer },
    };
    for (size_t i = 0; i < ARRAY_SIZE (atoms); i++)
    {
        *atoms[i].atom =
            MakeAtom (atoms[i].name, strlen (atoms[i].name), TRUE);
        if (!*atoms[i].atom)
            goto fail;
    }
    selections->window = create_window ();
    if (!selections->window ||
        !AddCallback (&SelectionCallback, selection_changed, selections))
        goto fail;
    selections->registered = TRUE;
    selections->dirty = TRUE;
    compositor.bridge_client->output = bridge_output;
    mb_data_set_bridge (&data_bridge, selections);
    return TRUE;
fail:
    mb_selections_shutdown ();
    return FALSE;
}

void
mb_selections_shutdown (void)
{
    struct MbSelections *selections = compositor.selections;
    Transfer *transfer, *next;
    ForeignWatch *watch, *next_watch;
    Import *import, *next_import;

    if (!selections)
        return;
    selections->stopping = TRUE;

    wl_list_for_each_safe (watch, next_watch, &selections->watches, link)
    {
        mb_selection_watch (watch->window, FALSE);
    }

    wl_list_for_each_safe (import, next_import, &selections->imports, link)
    {
        mb_data_source_destroy (import->source);
    }
    mb_data_set_bridge (NULL, NULL);
    if (selections->registered)
        DeleteCallback (&SelectionCallback, selection_changed, selections);
    compositor.bridge_client->output = NULL;
    wl_list_for_each_safe (transfer, next, &selections->transfers, link)
    {
        transfer_destroy (transfer);
    }
    if (lookup_window (selections->window, DixDestroyAccess))
        FreeResource (selections->window, RT_NONE);
    free (selections);
    compositor.selections = NULL;
}
