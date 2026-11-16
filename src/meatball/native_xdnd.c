/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "server.h"

#include <X11/Xatom.h>
#include <dixevents.h>
#include <propertyst.h>
#include <selection.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-protocol.h>
#include <xace.h>

#define XDND_VERSION 5
#define XDND_MIN_VERSION 3
#define XDND_VERSION_SHIFT 24
#define XDND_TYPE_LIST_FLAG 1
#define XDND_ACCEPT_FLAG 1
#define XDND_POSITION_FLAG 2
#define XDND_COORD_SHIFT 16
#define XDND_TYPES 256
#define XDND_INLINE_TYPES 3
#define XDND_STATUS_TIMEOUT_MS 10000
#define XDND_FINISH_TIMEOUT_MS 30000
#define SEND_EVENT_MASK 0x80
#define MIME_UTF8 "text/plain;charset=utf-8"
#define MIME_TEXT "text/plain"
#define DND_COPY WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY
#define DND_MOVE WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE
#define DND_ASK WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK
#define DND_ACTIONS (DND_COPY | DND_MOVE | DND_ASK)

typedef struct
{
    Atom aware, proxy, types, actions, enter, position, status, leave, drop;
    Atom finished, selection, copy, move, ask;
} XdndAtoms;

typedef struct
{
    uint64_t seat;
    MbDataSource *source;
    Window target, proxy;
    unsigned int version;
    MbDragPosition position;
    uint32_t action;
    Bool pending, dirty, drop_requested, drop_sent, failed;
} ExportDrag;

typedef struct
{
    uint64_t seat;
    MbDataSource *source;
    Window owner, target;
    MbSurface *surface;
    unsigned int version;
    uint32_t action, actions;
    Bool accepted, status, dropped, finished, success, clearing;
    Time time;
} ImportDrag;

struct MbXdnd
{
    XdndAtoms atoms;
    ExportDrag out;
    ImportDrag in;
    struct wl_event_source *timer;
};

static WindowPtr
native_window (Window id, Mask access)
{
    WindowPtr window;
    ClientPtr client = compositor.bridge_client->native;

    if (!id || !client ||
        dixLookupWindow (&window, id, client, access) != Success)
        return NULL;
    return window;
}

static PropertyPtr
native_property (Window id, Atom atom, Atom type, size_t limit)
{
    WindowPtr window = native_window (id, DixGetPropAccess);
    PropertyPtr property;

    if (!window ||
        dixLookupProperty (&property, window, atom,
                           compositor.bridge_client->native,
                           DixReadAccess) != Success ||
        !property || property->type != type ||
        property->format != MB_PROPERTY_FORMAT_32 || property->size > limit)
        return NULL;
    return property;
}

static Bool
send_message (Window destination, Window subject, Atom type, CARD32 first,
              CARD32 second, CARD32 third, CARD32 fourth, CARD32 fifth)
{
    WindowPtr window = native_window (destination, DixSendAccess);
    ClientPtr client = compositor.bridge_client->native;
    xEvent event = { 0 };

    if (!window)
        return FALSE;
    event.u.u.type = ClientMessage | SEND_EVENT_MASK;
    event.u.u.detail = MB_PROPERTY_FORMAT_32;
    event.u.clientMessage.window = subject;
    event.u.clientMessage.u.l.type = type;
    event.u.clientMessage.u.l.longs0 = first;
    event.u.clientMessage.u.l.longs1 = second;
    event.u.clientMessage.u.l.longs2 = third;
    event.u.clientMessage.u.l.longs3 = fourth;
    event.u.clientMessage.u.l.longs4 = fifth;
    if (XaceHookSendAccess (client, NULL, window, &event, 1) != Success)
        return FALSE;
    WriteEventsToClient (wClient (window), 1, &event);
    return TRUE;
}

static Atom
action_atom (uint32_t action)
{
    XdndAtoms *atoms = &compositor.xdnd->atoms;

    switch (action)
    {
    case DND_COPY:
        return atoms->copy;
    case DND_MOVE:
        return atoms->move;
    case DND_ASK:
        return atoms->ask;
    default:
        return None;
    }
}

static uint32_t
action_value (Atom atom)
{
    XdndAtoms *atoms = &compositor.xdnd->atoms;

    if (atom == atoms->copy)
        return DND_COPY;
    if (atom == atoms->move)
        return DND_MOVE;
    if (atom == atoms->ask)
        return DND_ASK;
    return WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
}

static void
clear_outgoing (void)
{
    struct MbXdnd *xdnd = compositor.xdnd;
    ExportDrag *out = &xdnd->out;

    if (out->target)
    {
        if (!out->drop_sent)
            send_message (out->proxy, out->target, xdnd->atoms.leave,
                          mb_selection_window (), 0, 0, 0, 0);
        mb_selection_watch (out->target, FALSE);
        if (out->proxy != out->target)
            mb_selection_watch (out->proxy, FALSE);
    }
    memset (out, 0, sizeof (*out));
    mb_selection_claim_drag (NULL);
    wl_event_source_timer_update (xdnd->timer, 0);
}

static void
clear_incoming (void)
{
    struct MbXdnd *xdnd = compositor.xdnd;
    ImportDrag *in = &xdnd->in;

    in->clearing = TRUE;
    if (in->source)
        mb_data_source_destroy (in->source);
    if (in->owner)
        mb_selection_watch (in->owner, FALSE);
    memset (in, 0, sizeof (*in));
    wl_event_source_timer_update (xdnd->timer, 0);
}

static int
drag_timeout (void *data)
{
    struct MbXdnd *xdnd = data;

    if (xdnd->out.seat)
        xdnd->out.failed = TRUE;
    if (xdnd->in.seat)
    {
        xdnd->in.finished = TRUE;
        xdnd->in.success = FALSE;
    }
    return 0;
}

static Bool
find_target (Window id, Window *target, Window *proxy, unsigned int *version)
{
    struct MbXdnd *xdnd = compositor.xdnd;
    WindowPtr window = native_window (id, DixGetAttrAccess);

    while (window)
    {
        Window candidate = window->drawable.id;
        Window recipient = candidate;
        MbSurface *surface;

        wl_list_for_each (surface, &compositor.surfaces, link)
        {
            if (surface->window.id == candidate)
                return FALSE;
        }
        PropertyPtr property =
            native_property (candidate, xdnd->atoms.proxy, XA_WINDOW, 1);

        if (property && property->size == 1)
        {
            Window proposed = *(CARD32 *)property->data;

            property =
                native_property (proposed, xdnd->atoms.proxy, XA_WINDOW, 1);
            if (property && property->size == 1 &&
                *(CARD32 *)property->data == proposed)
                recipient = proposed;
        }
        property = native_property (recipient, xdnd->atoms.aware, XA_ATOM, 1);
        if (property && property->size == 1 &&
            *(CARD32 *)property->data >= XDND_MIN_VERSION)
        {
            *target = candidate;
            *proxy = recipient;
            *version = min (*(CARD32 *)property->data, XDND_VERSION);
            return TRUE;
        }
        window = window->parent;
    }
    return FALSE;
}

static void
send_position (void)
{
    struct MbXdnd *xdnd = compositor.xdnd;
    ExportDrag *out = &xdnd->out;
    uint32_t actions = mb_data_source_actions (out->source);
    uint32_t preferred = actions & DND_COPY   ? DND_COPY
                         : actions & DND_MOVE ? DND_MOVE
                                              : DND_ASK;
    CARD32 coordinates = (CARD32)(uint16_t)out->position.root_x
                             << XDND_COORD_SHIFT |
                         (uint16_t)out->position.root_y;

    out->pending = TRUE;
    out->dirty = FALSE;
    if (!send_message (out->proxy, out->target, xdnd->atoms.position,
                       mb_selection_window (), 0, coordinates,
                       out->position.time, action_atom (preferred)))
        out->failed = TRUE;
    wl_event_source_timer_update (xdnd->timer, XDND_STATUS_TIMEOUT_MS);
}

static Bool
send_enter (void)
{
    struct MbXdnd *xdnd = compositor.xdnd;
    ExportDrag *out = &xdnd->out;
    CARD32 types[XDND_TYPES + XDND_INLINE_TYPES] = { 0 };
    CARD32 actions[XDND_INLINE_TYPES];
    const char *const *mimes;
    size_t count, length = 0, action_count = 0;
    uint32_t mask = mb_data_source_actions (out->source);
    WindowPtr window =
        native_window (mb_selection_window (), DixSetPropAccess);
    ClientPtr client = compositor.bridge_client->native;
    Bool text = FALSE;

    if (!window)
        return FALSE;
    mimes = mb_data_source_mimes (out->source, &count);
    for (size_t i = 0; i < count && length < XDND_TYPES; i++)
    {
        types[length++] = MakeAtom (mimes[i], strlen (mimes[i]), TRUE);
        if (!strcmp (mimes[i], MIME_UTF8) || !strcmp (mimes[i], MIME_TEXT))
            text = TRUE;
    }
    if (text && length < XDND_TYPES)
        types[length++] = compositor.atoms.utf8_string;
    for (uint32_t action = DND_COPY; action <= DND_ASK; action <<= 1)
        if (mask & action)
            actions[action_count++] = action_atom (action);
    if (dixChangeWindowProperty (client, window, xdnd->atoms.types, XA_ATOM,
                                 MB_PROPERTY_FORMAT_32, PropModeReplace,
                                 length, types, TRUE) != Success ||
        dixChangeWindowProperty (client, window, xdnd->atoms.actions, XA_ATOM,
                                 MB_PROPERTY_FORMAT_32, PropModeReplace,
                                 action_count, actions, TRUE) != Success)
        return FALSE;
    return send_message (
        out->proxy, out->target, xdnd->atoms.enter, mb_selection_window (),
        out->version << XDND_VERSION_SHIFT |
            (length > XDND_INLINE_TYPES ? XDND_TYPE_LIST_FLAG : 0),
        types[0], types[1], types[2]);
}

Bool
mb_xdnd_motion (uint64_t seat, MbDataSource *source,
                const MbDragPosition *position)
{
    struct MbXdnd *xdnd = compositor.xdnd;
    ExportDrag *out;
    Window target = None, proxy = None;
    unsigned int version = 0;

    if (!xdnd || !source || xdnd->in.seat)
        return FALSE;
    out = &xdnd->out;
    if (out->seat && (out->seat != seat || out->source != source))
        return FALSE;
    if (!position->surface)
        find_target (position->native, &target, &proxy, &version);
    if (out->seat && out->target != target)
        clear_outgoing ();
    if (!target)
        return FALSE;
    if (!out->seat)
    {
        out->seat = seat;
        out->source = source;
        out->target = target;
        out->proxy = proxy;
        out->version = version;
        if (!mb_selection_claim_drag (source) ||
            !mb_selection_watch (target, TRUE) ||
            (proxy != target && !mb_selection_watch (proxy, TRUE)) ||
            !send_enter ())
            out->failed = TRUE;
        out->dirty = TRUE;
    }
    if (out->position.root_x != position->root_x ||
        out->position.root_y != position->root_y)
        out->dirty = TRUE;
    out->position = *position;
    if (!out->pending && out->dirty && !out->drop_sent && !out->failed)
        send_position ();
    return TRUE;
}

Bool
mb_xdnd_drop (uint64_t seat)
{
    ExportDrag *out = &compositor.xdnd->out;

    if (out->seat != seat || out->failed)
        return FALSE;
    out->drop_requested = TRUE;
    return TRUE;
}

void
mb_xdnd_cancel (uint64_t seat)
{
    if (compositor.xdnd && compositor.xdnd->out.seat == seat)
        clear_outgoing ();
}

static void
incoming_notify (void *data, MbSourceEvent event, const char *mime,
                 uint32_t action)
{
    ImportDrag *in = data;

    if (in->clearing)
        return;
    switch (event)
    {
    case MB_SOURCE_TARGET:
        in->accepted = mime != NULL;
        in->status = TRUE;
        break;
    case MB_SOURCE_ACTION:
        in->action = action;
        in->status = TRUE;
        break;
    case MB_SOURCE_FINISHED:
        in->finished = TRUE;
        in->success = TRUE;
        break;
    case MB_SOURCE_CANCELLED:
        in->finished = TRUE;
        in->success = FALSE;
        break;
    case MB_SOURCE_DESTROYED:
        in->source = NULL;
        in->finished = TRUE;
        break;
    case MB_SOURCE_DROP:
        break;
    }
}

static void
receive_enter (MbSurface *surface, const xEvent *event)
{
    struct MbXdnd *xdnd = compositor.xdnd;
    ImportDrag *in = &xdnd->in;
    Window owner = event->u.clientMessage.u.l.longs0;
    CARD32 flags = event->u.clientMessage.u.l.longs1;
    unsigned int version = flags >> XDND_VERSION_SHIFT;
    PropertyPtr property;
    CARD32 types[XDND_TYPES];
    size_t count = XDND_INLINE_TYPES;
    uint32_t actions = 0;
    Selection *selection;

    if (!surface || xdnd->out.seat || version < XDND_MIN_VERSION ||
        !native_window (owner, DixGetAttrAccess) ||
        dixLookupSelection (&selection, xdnd->atoms.selection,
                            compositor.bridge_client->native,
                            DixReadAccess) != Success ||
        selection->window != owner)
        return;
    if (in->seat)
    {
        if (in->owner != owner || in->dropped)
            return;
        clear_incoming ();
    }
    types[0] = event->u.clientMessage.u.l.longs2;
    types[1] = event->u.clientMessage.u.l.longs3;
    types[2] = event->u.clientMessage.u.l.longs4;
    if (flags & XDND_TYPE_LIST_FLAG)
    {
        property =
            native_property (owner, xdnd->atoms.types, XA_ATOM, XDND_TYPES);
        if (!property || !property->size)
            return;
        count = property->size;
        memcpy (types, property->data, count * sizeof (*types));
    }
    property =
        native_property (owner, xdnd->atoms.actions, XA_ATOM, XDND_TYPES);
    if (property)
        for (size_t i = 0; i < property->size; i++)
            actions |= action_value (((CARD32 *)property->data)[i]);
    else
        actions = DND_ACTIONS;
    in->seat = mb_input_drag_seat (selection->client);
    in->owner = owner;
    in->target = surface->window.id;
    in->surface = surface;
    in->version = min (version, XDND_VERSION);
    in->actions = actions;
    in->source =
        mb_selection_import (owner, xdnd->atoms.selection, CurrentTime, types,
                             count, actions, incoming_notify, in);
    if (!in->source || !in->seat || !mb_selection_watch (owner, TRUE) ||
        !mb_data_drag_import (in->seat, in->source))
        clear_incoming ();
}

Bool
mb_xdnd_event (MbSurface *surface, const xEvent *event)
{
    struct MbXdnd *xdnd = compositor.xdnd;
    Atom type;
    Window sender;
    int kind = event->u.u.type & ~SEND_EVENT_MASK;
    ImportDrag *in;
    ExportDrag *out;

    if (!xdnd)
        return FALSE;
    in = &xdnd->in;
    out = &xdnd->out;
    if (kind == DestroyNotify)
    {
        Window destroyed = event->u.destroyNotify.window;

        /* A queued event may refer to an XID already reused by a client. */
        if ((event->u.u.type & SEND_EVENT_MASK) ||
            native_window (destroyed, DixGetAttrAccess))
            return FALSE;
        if (destroyed == out->target || destroyed == out->proxy)
            out->failed = TRUE;
        if (destroyed == in->owner || destroyed == in->target)
        {
            in->finished = TRUE;
            in->success = FALSE;
        }
        return FALSE;
    }
    if (kind == SelectionClear &&
        event->u.selectionClear.atom == xdnd->atoms.selection)
    {
        Selection *selection;

        if (out->seat &&
            (dixLookupSelection (&selection, xdnd->atoms.selection,
                                 compositor.bridge_client->native,
                                 DixReadAccess) != Success ||
             selection->window != mb_selection_window ()))
            out->failed = TRUE;
        return TRUE;
    }
    if (kind != ClientMessage || event->u.u.detail != MB_PROPERTY_FORMAT_32)
        return FALSE;
    type = event->u.clientMessage.u.l.type;
    sender = event->u.clientMessage.u.l.longs0;
    if (type == xdnd->atoms.status || type == xdnd->atoms.finished)
    {
        if (!out->seat || sender != out->target ||
            event->u.clientMessage.window != mb_selection_window ())
            return TRUE;
        if (type == xdnd->atoms.status && out->pending && !out->drop_sent)
        {
            out->pending = FALSE;
            uint32_t action = action_value (event->u.clientMessage.u.l.longs4);

            out->action = event->u.clientMessage.u.l.longs1 & XDND_ACCEPT_FLAG
                              ? action & mb_data_source_actions (out->source)
                              : 0;
            mb_data_drag_native_action (out->seat, out->action);
            wl_event_source_timer_update (xdnd->timer, 0);
        }
        else if (type == xdnd->atoms.finished && out->drop_sent)
        {
            uint32_t final =
                out->version < XDND_VERSION
                    ? out->action
                    : action_value (event->u.clientMessage.u.l.longs2);
            Bool success =
                out->version < XDND_VERSION ||
                ((event->u.clientMessage.u.l.longs1 & XDND_ACCEPT_FLAG) &&
                 (out->action == DND_ASK
                      ? final != DND_ASK &&
                            (final & mb_data_source_actions (out->source))
                      : final == out->action));
            uint64_t seat = out->seat;

            if (success && out->action == DND_ASK)
                mb_data_drag_native_action (seat, final);
            mb_data_drag_native_finish (seat, success);
        }
        return TRUE;
    }
    if (type == xdnd->atoms.enter)
    {
        receive_enter (surface, event);
        return TRUE;
    }
    if (type != xdnd->atoms.position && type != xdnd->atoms.leave &&
        type != xdnd->atoms.drop)
        return FALSE;
    if (!surface || !in->seat || in->owner != sender ||
        in->target != surface->window.id || in->dropped)
    {
        if (surface && type == xdnd->atoms.position)
            send_message (sender, sender, xdnd->atoms.status,
                          surface->window.id, XDND_POSITION_FLAG, 0, 0, None);
        return TRUE;
    }
    if (type == xdnd->atoms.leave)
        clear_incoming ();
    else if (type == xdnd->atoms.position)
    {
        CARD32 coordinates = event->u.clientMessage.u.l.longs2;
        int32_t x = (int16_t)(coordinates >> XDND_COORD_SHIFT);
        int32_t y = (int16_t)coordinates;
        MbDragPosition position = mb_input_drag_position (
            surface, x, y, event->u.clientMessage.u.l.longs3);

        uint32_t action = action_value (event->u.clientMessage.u.l.longs4);

        mb_data_drag_source_actions (
            in->seat, action == DND_ASK ? in->actions : action & in->actions);
        in->time = position.time;
        mb_selection_time (in->source, in->time);
        in->status = TRUE;
        mb_data_drag_motion (in->seat, &position);
    }
    else
    {
        in->dropped = TRUE;
        in->time = event->u.clientMessage.u.l.longs2;
        mb_selection_time (in->source, in->time);
        mb_data_drag_drop (in->seat);
        wl_event_source_timer_update (xdnd->timer, XDND_FINISH_TIMEOUT_MS);
    }
    return TRUE;
}

void
mb_xdnd_dispatch (void)
{
    struct MbXdnd *xdnd = compositor.xdnd;
    ExportDrag *out;
    ImportDrag *in;

    if (!xdnd)
        return;
    out = &xdnd->out;
    in = &xdnd->in;
    if (in->seat)
    {
        Selection *selection;

        if (dixLookupSelection (&selection, xdnd->atoms.selection,
                                compositor.bridge_client->native,
                                DixReadAccess) != Success ||
            selection->window != in->owner)
        {
            in->finished = TRUE;
            in->success = FALSE;
        }
    }
    if (out->seat && out->failed)
    {
        mb_data_drag_native_finish (out->seat, FALSE);
        if (out->seat)
            clear_outgoing ();
    }
    if (out->seat && !out->pending && !out->drop_sent)
    {
        if (out->dirty)
            send_position ();
        else if (out->drop_requested)
        {
            if (!out->action)
                mb_data_drag_native_finish (out->seat, FALSE);
            else
            {
                out->drop_sent = TRUE;
                if (!send_message (out->proxy, out->target, xdnd->atoms.drop,
                                   mb_selection_window (), 0,
                                   out->position.time, 0, 0))
                    out->failed = TRUE;
                wl_event_source_timer_update (xdnd->timer,
                                              XDND_FINISH_TIMEOUT_MS);
            }
        }
    }
    if (in->seat && in->finished)
    {
        Bool report_success = in->version >= XDND_VERSION && in->success;

        if (in->dropped)
            send_message (in->owner, in->owner, xdnd->atoms.finished,
                          in->target, report_success ? XDND_ACCEPT_FLAG : 0,
                          report_success ? action_atom (in->action) : None, 0,
                          0);
        clear_incoming ();
    }
    else if (in->seat && in->status && !in->dropped)
    {
        Bool accepted = in->accepted && in->action;

        send_message (in->owner, in->owner, xdnd->atoms.status, in->target,
                      XDND_POSITION_FLAG | (accepted ? XDND_ACCEPT_FLAG : 0),
                      0, 0, accepted ? action_atom (in->action) : None);
        in->status = FALSE;
    }
}

void
mb_xdnd_surface_destroyed (MbSurface *surface)
{
    if (compositor.xdnd && compositor.xdnd->in.seat &&
        compositor.xdnd->in.surface == surface)
    {
        compositor.xdnd->in.surface = NULL;
        compositor.xdnd->in.finished = TRUE;
        compositor.xdnd->in.success = FALSE;
    }
}

Bool
mb_xdnd_advertise (MbSurface *surface)
{
    WindowPtr window = mb_window_get (&surface->window, DixSetPropAccess);
    CARD32 version = XDND_VERSION;

    return window && compositor.xdnd &&
           dixChangeWindowProperty (surface->client->native, window,
                                    compositor.xdnd->atoms.aware, XA_ATOM,
                                    MB_PROPERTY_FORMAT_32, PropModeReplace, 1,
                                    &version, TRUE) == Success;
}

Bool
mb_xdnd_initialize (void)
{
    struct MbXdnd *xdnd = calloc (1, sizeof (*xdnd));

    if (!xdnd)
        return FALSE;
    compositor.xdnd = xdnd;
    struct
    {
        const char *name;
        Atom *atom;
    } atoms[] = {
        { "XdndAware", &xdnd->atoms.aware },
        { "XdndProxy", &xdnd->atoms.proxy },
        { "XdndTypeList", &xdnd->atoms.types },
        { "XdndActionList", &xdnd->atoms.actions },
        { "XdndEnter", &xdnd->atoms.enter },
        { "XdndPosition", &xdnd->atoms.position },
        { "XdndStatus", &xdnd->atoms.status },
        { "XdndLeave", &xdnd->atoms.leave },
        { "XdndDrop", &xdnd->atoms.drop },
        { "XdndFinished", &xdnd->atoms.finished },
        { "XdndSelection", &xdnd->atoms.selection },
        { "XdndActionCopy", &xdnd->atoms.copy },
        { "XdndActionMove", &xdnd->atoms.move },
        { "XdndActionAsk", &xdnd->atoms.ask },
    };
    for (size_t i = 0; i < ARRAY_SIZE (atoms); i++)
    {
        *atoms[i].atom =
            MakeAtom (atoms[i].name, strlen (atoms[i].name), TRUE);
        if (!*atoms[i].atom)
            goto fail;
    }
    xdnd->timer =
        wl_event_loop_add_timer (compositor.wl_event_loop, drag_timeout, xdnd);
    if (!xdnd->timer)
        goto fail;
    return TRUE;
fail:
    mb_xdnd_shutdown ();
    return FALSE;
}

void
mb_xdnd_shutdown (void)
{
    struct MbXdnd *xdnd = compositor.xdnd;

    if (!xdnd)
        return;
    if (xdnd->out.seat)
        clear_outgoing ();
    if (xdnd->in.seat)
        clear_incoming ();
    if (xdnd->timer)
        wl_event_source_remove (xdnd->timer);
    free (xdnd);
    compositor.xdnd = NULL;
}
