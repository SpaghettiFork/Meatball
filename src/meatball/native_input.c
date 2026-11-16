/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "server.h"

#include <X11/extensions/XI2.h>
#include <X11/extensions/XI2proto.h>
#include <exevents.h>
#include <extnsionst.h>
#include <inputstr.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server-protocol.h>
#include <window.h>
#include <xkbsrv.h>

#define SEAT_VERSION 5
#define SEAT_NAME_FORMAT "spaghetti-seat-%d-%d"
#define SEAT_NAME_SIZE 64
#define INPUT_QUEUE_LIMIT (1024 * 1024)
#define INPUT_DISPATCH_LIMIT 256
#define XORG_KEYCODE_OFFSET 8
#define KEYCODE_COUNT 256
#define X11_SCROLL_UP 4
#define X11_SCROLL_DOWN 5
#define X11_SCROLL_LEFT 6
#define X11_SCROLL_RIGHT 7
#define X11_SIDE_BUTTON 8
#define X11_LEFT_BUTTON 1
#define X11_MIDDLE_BUTTON 2
#define X11_RIGHT_BUTTON 3
#define SCROLL_STEP 10
#define FIXED_16_ONE 65536.0
#define MILLISECONDS_PER_SECOND 1000
#define INVALID_KEYMAP_FD (-1)
#define INPUT_SERIAL_LIMIT 128

enum ResourceKind
{
    RESOURCE_SEAT,
    RESOURCE_POINTER,
    RESOURCE_KEYBOARD,
};

enum InputKind
{
    INPUT_XI,
    INPUT_STATE,
    INPUT_KEYMAP,
    INPUT_CONTROLS,
};

struct Modifiers
{
    uint32_t depressed, latched, locked, group;
};

struct InputSerial
{
    uint32_t serial;
    uint64_t client;
};

struct MbSeat
{
    struct wl_list link, resources;
    struct wl_global *global;
    uint64_t lifetime;
    int pointer_id, keyboard_id;
    Bool seen, removed, retire_pending;
    uint32_t capabilities, had_capabilities;
    char name[SEAT_NAME_SIZE];
    int keymap_fd;
    uint32_t keymap_size;
    XID keyboard_subscription;
    int32_t repeat_rate, repeat_delay;
    MbSurface *pointer_focus, *keyboard_focus, *grab_focus, *pointer_root;
    uint32_t enter_serial, button_serial;
    struct InputSerial serials[INPUT_SERIAL_LIMIT];
    unsigned int serial_index;
    uint64_t drag_owner;
    uint32_t drag_enter_serial;
    ScreenPtr drag_screen;
    unsigned char keys[DOWN_LENGTH], buttons[DOWN_LENGTH];
    unsigned int button_count, grab_button;
    wl_fixed_t pointer_x, pointer_y;
    int32_t root_x, root_y;
    double root_pointer_x, root_pointer_y;
    struct Modifiers modifiers;
    MbSurface *cursor_surface;
    uint64_t cursor_owner;
    XID cursor;
    int64_t hotspot_x, hotspot_y;
};

struct InputResource
{
    struct wl_list link;
    struct wl_resource *resource;
    struct MbSeat *seat;
    MbClient *client;
    enum ResourceKind kind;
};

struct InputEvent
{
    struct wl_list link;
    enum InputKind kind;
    uint64_t client, seat;
    uint64_t target_client;
    XID target_window;
    int target_screen;
    Bool drag_event;
    size_t size, charge;
    unsigned char keys[DOWN_LENGTH];
    struct Modifiers modifiers;
    int keymap_fd;
    uint32_t keymap_size;
    int32_t repeat_rate, repeat_delay;
    unsigned char data[];
};

struct MbInput
{
    struct wl_list seats, events;
    size_t queued;
    int xi_opcode;
    Bool callback_registered;
};

static uint64_t next_seat_lifetime;

static MbSurface *root_for_window (WindowPtr);
static void pointer_focus (struct MbSeat *, MbSurface *, wl_fixed_t,
                           wl_fixed_t);
static void keyboard_focus (struct MbSeat *, MbSurface *,
                            const unsigned char *);
static void bind_seat (struct wl_client *, void *, uint32_t, uint32_t);
static void pointer_motion (struct MbSeat *, MbSurface *, double, double,
                            uint32_t, Bool);

static struct MbSeat *
find_seat (int device_id)
{
    struct MbSeat *seat;

    wl_list_for_each (seat, &compositor.input->seats, link)
    {
        if (!seat->removed && !seat->retire_pending &&
            (seat->pointer_id == device_id || seat->keyboard_id == device_id))
            return seat;
    }
    return NULL;
}

static struct MbSeat *
find_seat_id (uint64_t id)
{
    struct MbSeat *seat;

    if (!compositor.input)
        return NULL;
    wl_list_for_each (seat, &compositor.input->seats, link)
    {
        if (seat->lifetime == id)
            return seat;
    }
    return NULL;
}

static void
record_serial (struct MbSeat *seat, MbClient *client, uint32_t serial)
{
    unsigned int previous =
        (seat->serial_index + INPUT_SERIAL_LIMIT - 1) % INPUT_SERIAL_LIMIT;

    if (seat->serials[previous].serial == serial &&
        seat->serials[previous].client == client->lifetime)
        return;
    seat->serials[seat->serial_index] =
        (struct InputSerial){ serial, client->lifetime };
    seat->serial_index = (seat->serial_index + 1) % INPUT_SERIAL_LIMIT;
}

MbClient *
mb_input_keyboard_client (uint64_t id)
{
    struct MbSeat *seat = find_seat_id (id);

    if (!seat || seat->removed || seat->retire_pending ||
        !seat->keyboard_focus || seat->keyboard_focus->client->failed)
        return NULL;
    return seat->keyboard_focus->client;
}

Bool
mb_input_selection_serial (uint64_t id, MbClient *client, uint32_t serial)
{
    struct MbSeat *seat = find_seat_id (id);
    unsigned int i;

    if (!seat || mb_input_keyboard_client (id) != client)
        return FALSE;
    for (i = 0; i < INPUT_SERIAL_LIMIT; i++)
        if (seat->serials[i].serial == serial &&
            seat->serials[i].client == client->lifetime)
            return TRUE;
    return FALSE;
}

static void
resource_destroyed (struct wl_resource *resource)
{
    struct InputResource *input = wl_resource_get_user_data (resource);

    wl_list_remove (&input->link);
    free (input);
}

static void
release (struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_destroy (resource);
}

static struct InputResource *
create_resource (struct wl_client *client, struct MbSeat *seat,
                 enum ResourceKind kind, const struct wl_interface *interface,
                 uint32_t version, uint32_t id, const void *implementation)
{
    struct InputResource *input = calloc (1, sizeof (*input));

    if (!input)
        return NULL;
    input->resource = wl_resource_create (client, interface, version, id);
    if (!input->resource)
    {
        free (input);
        return NULL;
    }
    input->kind = kind;
    input->seat = seat;
    input->client = mb_client_for_wayland (client);
    wl_list_insert (seat->resources.prev, &input->link);
    wl_resource_set_implementation (input->resource, implementation, input,
                                    resource_destroyed);
    return input;
}

static void
send_pointer_frame (struct InputResource *input)
{
    if (wl_resource_get_version (input->resource) >=
        WL_POINTER_FRAME_SINCE_VERSION)
        wl_pointer_send_frame (input->resource);
}

static void
send_keyboard_enter (struct InputResource *input, uint32_t serial)
{
    struct MbSeat *seat = input->seat;
    struct wl_array keys;
    uint32_t pressed[KEYCODE_COUNT];
    unsigned int key, count = 0;

    for (key = XORG_KEYCODE_OFFSET; key < KEYCODE_COUNT; key++)
        if (BitIsOn (seat->keys, key))
            pressed[count++] = key - XORG_KEYCODE_OFFSET;
    keys = (struct wl_array){ .size = count * sizeof (*pressed),
                              .data = pressed };
    record_serial (seat, input->client, serial);
    wl_keyboard_send_enter (input->resource, serial,
                            seat->keyboard_focus->resource, &keys);
    wl_keyboard_send_modifiers (input->resource, serial,
                                seat->modifiers.depressed,
                                seat->modifiers.latched,
                                seat->modifiers.locked, seat->modifiers.group);
}

static void
clear_cursor (struct MbSeat *seat)
{
    if (seat->cursor && mb_client_lookup (seat->cursor_owner))
        FreeResource (seat->cursor, RT_NONE);
    seat->cursor = None;
    seat->cursor_owner = 0;
    seat->cursor_surface = NULL;
}

static Bool
update_cursor (struct MbSeat *seat)
{
    MbClient *client = mb_client_lookup (seat->cursor_owner);
    MbSurface *root;
    WindowPtr window;
    DeviceIntPtr pointer;
    CursorPtr cursor;

    if (!client || seat->removed)
        return TRUE;
    if (dixLookupDevice (&pointer, seat->pointer_id, client->native,
                         DixUseAccess) != Success)
        return FALSE;
    if (seat->drag_owner)
    {
        if (seat->drag_owner != client->lifetime ||
            !pointer->deviceGrab.grab ||
            CLIENT_ID (pointer->deviceGrab.grab->resource) !=
                client->native->index)
            return TRUE;
        window = pointer->deviceGrab.grab->window;
    }
    else
    {
        if (!seat->pointer_focus || seat->pointer_focus->client != client)
            return TRUE;
        root = mb_subsurface_root (seat->pointer_focus);
        if (!root)
            return TRUE;
        window = mb_window_get (&root->window, DixSetAttrAccess);
        if (!window)
            return FALSE;
    }
    cursor = mb_cursor_create (client, seat->cursor_surface, seat->hotspot_x,
                               seat->hotspot_y);
    if (!cursor)
        return FALSE;
    if (seat->drag_owner)
    {
        CursorPtr previous = pointer->deviceGrab.grab->cursor;

        pointer->deviceGrab.grab->cursor = RefCursor (cursor);
        if (previous)
            FreeCursor (previous, None);
        WindowHasNewCursor (window);
    }
    else if (ChangeWindowDeviceCursor (window, pointer, cursor) != Success)
    {
        FreeResource (cursor->id, RT_NONE);
        return FALSE;
    }
    if (seat->cursor)
        FreeResource (seat->cursor, RT_NONE);
    seat->cursor = cursor->id;
    return TRUE;
}

static void
set_cursor (struct wl_client *client, struct wl_resource *resource,
            uint32_t serial, struct wl_resource *surface_resource, int32_t x,
            int32_t y)
{
    struct InputResource *input = wl_resource_get_user_data (resource);
    struct MbSeat *seat = input->seat;
    MbSurface *surface =
        surface_resource ? wl_resource_get_user_data (surface_resource) : NULL;

    if (seat->drag_owner)
    {
        if (seat->drag_owner != input->client->lifetime ||
            serial != seat->drag_enter_serial)
            return;
    }
    else if (!seat->pointer_focus ||
             seat->pointer_focus->client != input->client ||
             serial != seat->enter_serial)
        return;
    if (surface && surface->role != MB_ROLE_NONE &&
        surface->role != MB_ROLE_CURSOR)
    {
        wl_resource_post_error (resource, WL_POINTER_ERROR_ROLE,
                                "cursor surface already has a role");
        return;
    }
    clear_cursor (seat);
    seat->cursor_owner = input->client->lifetime;
    seat->cursor_surface = surface;
    seat->hotspot_x = x;
    seat->hotspot_y = y;
    if (surface)
    {
        surface->role = MB_ROLE_CURSOR;
        mb_surface_update (surface);
    }
    else if (!update_cursor (seat))
        wl_resource_post_no_memory (resource);
}

Bool
mb_input_cursor_intersects (MbSurface *cursor, ScreenPtr screen,
                            const pixman_box32_t *node,
                            const pixman_box32_t *output)
{
    struct MbSeat *seat;

    if (!compositor.input)
        return FALSE;
    wl_list_for_each (seat, &compositor.input->seats, link)
    {
        MbSurface *root;
        double x, y;

        if (seat->removed || seat->cursor_surface != cursor)
            continue;
        if (seat->drag_owner)
        {
            if (seat->drag_screen != screen)
                continue;
        }
        else
        {
            root = seat->pointer_focus
                       ? mb_subsurface_root (seat->pointer_focus)
                       : NULL;
            if (!root || root->window.screen != screen)
                continue;
        }
        x = seat->root_pointer_x - seat->hotspot_x + screen->x;
        y = seat->root_pointer_y - seat->hotspot_y + screen->y;
        if (x + node->x1 < output->x2 && x + node->x2 > output->x1 &&
            y + node->y1 < output->y2 && y + node->y2 > output->y1)
            return TRUE;
    }
    return FALSE;
}

Bool
mb_input_cursor_visible (MbSurface *surface)
{
    struct MbSeat *seat;

    if (!compositor.input)
        return FALSE;
    wl_list_for_each (seat, &compositor.input->seats, link)
    {
        if (!seat->removed && seat->cursor_surface == surface &&
            (seat->drag_owner == surface->client->lifetime ||
             (seat->pointer_focus &&
              seat->pointer_focus->client == surface->client)))
            return TRUE;
    }
    return FALSE;
}

Bool
mb_input_cursor_changed (MbSurface *surface)
{
    struct MbSeat *seat;

    if (!compositor.input || surface->role != MB_ROLE_CURSOR)
        return TRUE;
    wl_list_for_each (seat, &compositor.input->seats, link)
    {
        if (seat->cursor_surface != surface)
            continue;
        seat->hotspot_x -= surface->offset_x;
        seat->hotspot_y -= surface->offset_y;
        if (!update_cursor (seat))
            return FALSE;
    }
    return TRUE;
}

static const struct wl_pointer_interface pointer_implementation = {
    .set_cursor = set_cursor,
    .release = release,
};

static const struct wl_keyboard_interface keyboard_implementation = {
    .release = release,
};

static void
get_pointer (struct wl_client *client, struct wl_resource *resource,
             uint32_t id)
{
    struct InputResource *binding = wl_resource_get_user_data (resource);
    struct MbSeat *seat = binding->seat;
    DeviceIntPtr pointer;
    struct InputResource *input;

    if (!seat->removed &&
        dixLookupDevice (&pointer, seat->pointer_id, binding->client->native,
                         DixUseAccess) != Success)
    {
        wl_client_post_implementation_error (client,
                                             "pointer device access denied");
        return;
    }
    input = create_resource (
        client, seat, RESOURCE_POINTER, &wl_pointer_interface,
        wl_resource_get_version (resource), id, &pointer_implementation);

    if (!input)
    {
        wl_resource_post_no_memory (resource);
        return;
    }
    if (seat->pointer_focus && seat->pointer_focus->client == input->client)
    {
        record_serial (seat, input->client, seat->enter_serial);
        wl_pointer_send_enter (input->resource, seat->enter_serial,
                               seat->pointer_focus->resource, seat->pointer_x,
                               seat->pointer_y);
        send_pointer_frame (input);
    }
}

static void
get_keyboard (struct wl_client *client, struct wl_resource *resource,
              uint32_t id)
{
    struct InputResource *binding = wl_resource_get_user_data (resource);
    struct MbSeat *seat = binding->seat;
    struct InputResource *input;
    DeviceIntPtr keyboard;

    if (seat->keymap_fd == INVALID_KEYMAP_FD &&
        !(seat->removed &&
          (seat->had_capabilities & WL_SEAT_CAPABILITY_KEYBOARD)))
    {
        wl_resource_post_error (resource, WL_SEAT_ERROR_MISSING_CAPABILITY,
                                "seat has no keyboard");
        return;
    }
    if (!seat->removed &&
        dixLookupDevice (&keyboard, seat->keyboard_id, binding->client->native,
                         DixGetAttrAccess) != Success)
    {
        wl_client_post_implementation_error (client,
                                             "keyboard device access denied");
        return;
    }
    input = create_resource (
        client, seat, RESOURCE_KEYBOARD, &wl_keyboard_interface,
        wl_resource_get_version (resource), id, &keyboard_implementation);
    if (!input)
    {
        wl_resource_post_no_memory (resource);
        return;
    }
    if (seat->removed)
        return;
    wl_keyboard_send_keymap (input->resource, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
                             seat->keymap_fd, seat->keymap_size);
    if (wl_resource_get_version (input->resource) >=
        WL_KEYBOARD_REPEAT_INFO_SINCE_VERSION)
        wl_keyboard_send_repeat_info (input->resource, seat->repeat_rate,
                                      seat->repeat_delay);
    if (seat->keyboard_focus && seat->keyboard_focus->client == input->client)
        send_keyboard_enter (input,
                             wl_display_next_serial (compositor.wl_display));
}

static void
get_touch (struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
    wl_resource_post_error (resource, WL_SEAT_ERROR_MISSING_CAPABILITY,
                            "seat has no touch capability");
}

static const struct wl_seat_interface seat_implementation = {
    .get_pointer = get_pointer,
    .get_keyboard = get_keyboard,
    .get_touch = get_touch,
    .release = release,
};

uint64_t
mb_input_seat_id (struct wl_resource *resource)
{
    struct InputResource *input;

    if (!wl_resource_instance_of (resource, &wl_seat_interface,
                                  &seat_implementation))
        return 0;
    input = wl_resource_get_user_data (resource);
    return input->seat->lifetime;
}

Bool
mb_input_take_grab (MbSurface *surface, struct wl_resource *resource,
                    uint32_t serial, MbInteractiveGrab *result)
{
    struct InputResource *binding;
    struct MbSeat *seat;
    DeviceIntPtr pointer;
    GrabPtr grab;

    if (!wl_resource_instance_of (resource, &wl_seat_interface,
                                  &seat_implementation))
        return FALSE;
    binding = wl_resource_get_user_data (resource);
    seat = binding->seat;
    if (seat->removed || seat->retire_pending ||
        binding->client != surface->client || !seat->button_count ||
        serial != seat->button_serial || !seat->grab_focus ||
        mb_subsurface_root (seat->grab_focus) != surface ||
        !BitIsOn (seat->buttons, seat->grab_button) ||
        dixLookupDevice (&pointer, seat->pointer_id, surface->client->native,
                         DixGrabAccess) != Success)
        return FALSE;
    grab = pointer->deviceGrab.grab;
    if (!grab || !pointer->deviceGrab.implicitGrab ||
        CLIENT_ID (grab->resource) != surface->client->native->index)
        return FALSE;
    *result = (MbInteractiveGrab){ pointer, pointer->spriteInfo->sprite->hot.x,
                                   pointer->spriteInfo->sprite->hot.y,
                                   seat->grab_button };
    pointer->deviceGrab.DeactivateGrab (pointer);
    pointer_focus (seat, NULL, 0, 0);
    seat->grab_focus = seat->pointer_root = NULL;
    seat->button_serial = seat->button_count = 0;
    memset (seat->buttons, 0, sizeof (seat->buttons));
    return TRUE;
}

static void
bind_seat (struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    struct MbSeat *seat = data;
    struct InputResource *input =
        create_resource (client, seat, RESOURCE_SEAT, &wl_seat_interface,
                         version, id, &seat_implementation);

    if (!input)
    {
        wl_client_post_no_memory (client);
        return;
    }
    wl_seat_send_capabilities (input->resource, seat->capabilities);
    if (version >= WL_SEAT_NAME_SINCE_VERSION)
        wl_seat_send_name (input->resource, seat->name);
}

static void
pointer_focus (struct MbSeat *seat, MbSurface *surface, wl_fixed_t x,
               wl_fixed_t y)
{
    struct InputResource *input;
    uint32_t serial;

    seat->pointer_x = x;
    seat->pointer_y = y;
    if (seat->pointer_focus == surface)
        return;
    serial = wl_display_next_serial (compositor.wl_display);
    wl_list_for_each (input, &seat->resources, link)
    {
        if (input->kind != RESOURCE_POINTER || input->client->failed)
            continue;
        if (seat->pointer_focus &&
            seat->pointer_focus->client == input->client)
        {
            wl_pointer_send_leave (input->resource, serial,
                                   seat->pointer_focus->resource);
            send_pointer_frame (input);
        }
        if (surface && surface->client == input->client)
        {
            record_serial (seat, input->client, serial);
            wl_pointer_send_enter (input->resource, serial, surface->resource,
                                   x, y);
            send_pointer_frame (input);
        }
    }
    seat->pointer_focus = surface;
    seat->enter_serial = serial;
    if (seat->drag_owner)
    {
        if (!update_cursor (seat) && seat->cursor_surface)
            wl_resource_post_no_memory (seat->cursor_surface->resource);
    }
    else if (!surface || surface->client->lifetime != seat->cursor_owner)
        clear_cursor (seat);
    else if (!update_cursor (seat))
        wl_resource_post_no_memory (surface->resource);
}

static void
keyboard_focus (struct MbSeat *seat, MbSurface *surface,
                const unsigned char *keys)
{
    struct InputResource *input;
    MbSurface *previous = seat->keyboard_focus;
    uint32_t serial;

    if (previous == surface)
        return;
    serial = wl_display_next_serial (compositor.wl_display);
    seat->keyboard_focus = surface;
    memset (seat->keys, 0, sizeof (seat->keys));
    if (keys)
        memcpy (seat->keys, keys, sizeof (seat->keys));
    mb_data_keyboard_focus (seat->lifetime, surface ? surface->client : NULL);
    wl_list_for_each (input, &seat->resources, link)
    {
        if (input->kind != RESOURCE_KEYBOARD || input->client->failed)
            continue;
        if (previous && previous->client == input->client)
            wl_keyboard_send_leave (input->resource, serial,
                                    previous->resource);
        if (surface && surface->client == input->client)
            send_keyboard_enter (input, serial);
    }
}

static MbSurface *
window_surface (uint64_t client, XID window)
{
    MbSurface *surface;

    wl_list_for_each (surface, &compositor.surfaces, link)
    {
        if (surface->client->lifetime == client &&
            surface->window.id == window)
            return surface;
    }
    return NULL;
}

static void
modifiers_changed (struct MbSeat *seat, struct Modifiers modifiers)
{
    struct InputResource *input;
    uint32_t serial;

    if (!memcmp (&seat->modifiers, &modifiers, sizeof (modifiers)))
        return;
    seat->modifiers = modifiers;
    serial = wl_display_next_serial (compositor.wl_display);
    wl_list_for_each (input, &seat->resources, link)
    {
        if (input->kind == RESOURCE_KEYBOARD && seat->keyboard_focus &&
            input->client == seat->keyboard_focus->client &&
            !input->client->failed)
        {
            record_serial (seat, input->client, serial);
            wl_keyboard_send_modifiers (input->resource, serial,
                                        modifiers.depressed, modifiers.latched,
                                        modifiers.locked, modifiers.group);
        }
    }
}

static void
repeat_values (DeviceIntPtr keyboard, int32_t *rate, int32_t *delay)
{
    XkbControlsPtr controls = keyboard->key->xkbInfo->desc->ctrls;

    *rate = controls && controls->repeat_interval &&
                    (controls->enabled_ctrls & XkbRepeatKeysMask)
                ? MILLISECONDS_PER_SECOND / controls->repeat_interval
                : 0;
    *delay = controls ? controls->repeat_delay : 0;
}

static void
repeat_changed (struct MbSeat *seat, int32_t rate, int32_t delay)
{
    struct InputResource *input;

    if (seat->repeat_rate == rate && seat->repeat_delay == delay)
        return;
    seat->repeat_rate = rate;
    seat->repeat_delay = delay;
    wl_list_for_each (input, &seat->resources, link)
    {
        if (input->kind == RESOURCE_KEYBOARD && !input->client->failed &&
            wl_resource_get_version (input->resource) >=
                WL_KEYBOARD_REPEAT_INFO_SINCE_VERSION)
            wl_keyboard_send_repeat_info (input->resource, rate, delay);
    }
}

static struct InputEvent *
queue_event (MbClient *client, struct MbSeat *seat, enum InputKind kind,
             size_t size)
{
    struct MbInput *input = compositor.input;
    struct InputEvent *event;
    size_t charge = sizeof (*event) + size;

    if (size > INPUT_QUEUE_LIMIT || charge > INPUT_QUEUE_LIMIT - input->queued)
        goto fail;
    event = calloc (1, charge);
    if (!event)
        goto fail;
    event->client = client->lifetime;
    event->seat = seat ? seat->lifetime : 0;
    event->kind = kind;
    event->size = size;
    event->charge = charge;
    event->keymap_fd = INVALID_KEYMAP_FD;
    input->queued += charge;
    wl_list_insert (input->events.prev, &event->link);
    return event;

fail:
    mb_client_disconnect (client);
    return NULL;
}

static void
free_event (struct InputEvent *event)
{
    if (event->keymap_fd != INVALID_KEYMAP_FD)
        close (event->keymap_fd);
    compositor.input->queued -= event->charge;
    wl_list_remove (&event->link);
    free (event);
}

static Bool
queue_keymap (MbClient *client, struct MbSeat *seat, DeviceIntPtr keyboard)
{
    struct InputEvent *event;
    uint32_t size;
    int fd = mb_keymap_create (keyboard, &size);

    if (fd == INVALID_KEYMAP_FD)
        goto fail;
    event = queue_event (client, seat, INPUT_KEYMAP, 0);
    if (!event)
    {
        close (fd);
        return FALSE;
    }
    event->keymap_fd = fd;
    if (size > INPUT_QUEUE_LIMIT - compositor.input->queued)
    {
        free_event (event);
        goto fail;
    }
    event->keymap_size = size;
    event->modifiers =
        (struct Modifiers){ keyboard->key->xkbInfo->state.base_mods,
                            keyboard->key->xkbInfo->state.latched_mods,
                            keyboard->key->xkbInfo->state.locked_mods,
                            keyboard->key->xkbInfo->state.group };
    event->charge += size;
    compositor.input->queued += size;
    repeat_values (keyboard, &event->repeat_rate, &event->repeat_delay);
    return TRUE;

fail:
    mb_client_disconnect (client);
    return FALSE;
}

Bool
mb_input_observe_write (MbClient *client, const void *data, size_t size)
{
    const xkbAnyEvent *notification = data;
    struct MbSeat *seat;
    DeviceIntPtr keyboard;
    struct InputEvent *event;

    if (!compositor.input || client != compositor.bridge_client ||
        size != sizeof (xEvent) || notification->type != XkbEventBase)
        return TRUE;
    seat = find_seat (notification->deviceID);
    if (!seat)
        return TRUE;
    if (notification->xkbType == XkbStateNotify)
    {
        const xkbStateNotify *state = data;

        event = queue_event (client, seat, INPUT_STATE, 0);
        if (!event)
            return FALSE;
        event->modifiers =
            (struct Modifiers){ state->baseMods, state->latchedMods,
                                state->lockedMods, state->group };
        return TRUE;
    }
    if (dixLookupDevice (&keyboard, seat->keyboard_id, client->native,
                         DixGetAttrAccess) != Success ||
        !keyboard->key || !keyboard->key->xkbInfo)
        return TRUE;
    switch (notification->xkbType)
    {
    case XkbNewKeyboardNotify:
    case XkbMapNotify:
    case XkbNamesNotify:
    case XkbCompatMapNotify:
        return queue_keymap (client, seat, keyboard);
    case XkbControlsNotify:
        event = queue_event (client, seat, INPUT_CONTROLS, 0);
        if (!event)
            return FALSE;
        repeat_values (keyboard, &event->repeat_rate, &event->repeat_delay);
        break;
    }
    return TRUE;
}

static void
native_event (CallbackListPtr *callbacks, void *closure, void *data)
{
    EventInfoRec *info = data;
    const xXIGenericDeviceEvent *generic = (const void *)info->events;
    MbClient *client;
    struct MbSeat *seat;
    struct InputEvent *event;
    size_t size;

    if (info->count != 1 || generic->type != GenericEvent ||
        generic->extension != compositor.input->xi_opcode ||
        generic->length >
            (INPUT_QUEUE_LIMIT - sizeof (xEvent)) / sizeof (CARD32))
        return;
    size = sizeof (xEvent) + generic->length * sizeof (CARD32);
    wl_list_for_each (client, &compositor.clients, link)
    {
        if (client->native == info->client)
            break;
    }
    if (&client->link == &compositor.clients || client->failed)
        return;
    if (generic->evtype == XI_HierarchyChanged &&
        size >= sizeof (xXIHierarchyEvent))
    {
        const xXIHierarchyEvent *hierarchy = (const void *)generic;
        const xXIHierarchyInfo *devices = (const void *)(hierarchy + 1);
        unsigned int i;

        if (hierarchy->num_info >
            (size - sizeof (*hierarchy)) / sizeof (*devices))
            return;
        for (i = 0; i < hierarchy->num_info; i++)
        {
            if (!(devices[i].flags & (XIMasterRemoved | XIDeviceDisabled)))
                continue;
            seat = find_seat (devices[i].deviceid);
            if (seat)
                seat->retire_pending = TRUE;
        }
        return;
    }
    seat = find_seat (generic->deviceid);
    if (!seat)
        return;
    if (generic->evtype == XI_DeviceChanged)
    {
        DeviceIntPtr keyboard;

        if (generic->deviceid == seat->keyboard_id &&
            dixLookupDevice (&keyboard, seat->keyboard_id, client->native,
                             DixGetAttrAccess) == Success &&
            keyboard->key && keyboard->key->xkbInfo)
            queue_keymap (client, seat, keyboard);
        return;
    }
    if (generic->evtype < XI_KeyPress || generic->evtype > XI_FocusOut)
        return;
    event = queue_event (client, seat, INPUT_XI, size);
    if (!event)
        return;
    memcpy (event->data, generic, size);
    if (seat->drag_owner == client->lifetime &&
        generic->evtype >= XI_ButtonPress && generic->evtype <= XI_Motion)
    {
        DeviceIntPtr pointer;

        if (dixLookupDevice (&pointer, seat->pointer_id, client->native,
                             DixGetAttrAccess) == Success)
        {
            MbSurface *root = root_for_window (GetSpriteWindow (pointer));

            event->drag_event = TRUE;
            event->target_client = root ? root->client->lifetime : 0;
            event->target_window = root ? root->window.id : None;
            event->target_screen =
                pointer->spriteInfo->sprite->hotPhys.pScreen->myNum;
        }
    }
    if (generic->evtype == XI_FocusIn)
    {
        DeviceIntPtr keyboard;

        if (dixLookupDevice (&keyboard, seat->keyboard_id, client->native,
                             DixGetAttrAccess) == Success &&
            keyboard->key)
            memcpy (event->keys, keyboard->key->down, sizeof (event->keys));
    }
}

static void
retire_seat (struct MbSeat *seat)
{
    struct InputResource *resource;

    pointer_focus (seat, NULL, 0, 0);
    keyboard_focus (seat, NULL, NULL);
    seat->grab_focus = seat->pointer_root = NULL;
    seat->button_count = 0;
    seat->capabilities = 0;
    seat->removed = TRUE;
    mb_data_seat_removed (seat->lifetime);
    repeat_changed (seat, 0, seat->repeat_delay);
    wl_global_remove (seat->global);
    wl_list_for_each (resource, &seat->resources, link)
    {
        if (resource->kind == RESOURCE_SEAT && !resource->client->failed)
            wl_seat_send_capabilities (resource->resource, 0);
    }
    if (seat->keyboard_subscription && compositor.bridge_client->native)
        FreeResource (seat->keyboard_subscription, RT_NONE);
    seat->keyboard_subscription = None;
    if (seat->keymap_fd != INVALID_KEYMAP_FD)
        close (seat->keymap_fd);
    seat->keymap_fd = INVALID_KEYMAP_FD;
}

static MbSurface *
root_for_window (WindowPtr window)
{
    while (window && window != PointerRootWin && window != FollowKeyboardWin)
    {
        MbSurface *surface;

        wl_list_for_each (surface, &compositor.surfaces, link)
        {
            if (surface->window.id == window->drawable.id &&
                surface->role != MB_ROLE_DRAG_ICON &&
                surface->role != MB_ROLE_CURSOR && window->realized &&
                surface->pixels && mb_subsurface_root (surface) == surface)
                return surface;
        }
        window = window->parent;
    }
    return NULL;
}

static Bool
create_seat (DeviceIntPtr pointer, DeviceIntPtr keyboard)
{
    struct MbSeat *seat = calloc (1, sizeof (*seat));
    XkbInterestPtr interest;
    XkbStatePtr state;
    ClientPtr bridge = compositor.bridge_client->native;

    if (!seat)
        return FALSE;
    wl_list_init (&seat->resources);
    seat->pointer_id = pointer->id;
    seat->keyboard_id = keyboard->id;
    seat->lifetime = ++next_seat_lifetime;
    seat->seen = TRUE;
    seat->capabilities = WL_SEAT_CAPABILITY_POINTER;
    snprintf (seat->name, sizeof (seat->name), SEAT_NAME_FORMAT, pointer->id,
              keyboard->id);
    seat->keymap_fd = mb_keymap_create (keyboard, &seat->keymap_size);
    if (seat->keymap_fd != INVALID_KEYMAP_FD)
    {
        XID id = FakeClientID (bridge->index);

        interest = XkbAddClientResource ((DevicePtr)keyboard, bridge, id);
        if (!interest)
            goto fail;
        if (!AddResource (id, RT_XKBCLIENT, keyboard))
            goto fail;
        seat->keyboard_subscription = id;
        interest->stateNotifyMask = XkbAllStateComponentsMask;
        interest->ctrlsNotifyMask = XkbAllControlsMask;
        interest->namesNotifyMask = XkbAllNamesMask;
        interest->compatNotifyMask = XkbAllCompatMask;
        seat->capabilities |= WL_SEAT_CAPABILITY_KEYBOARD;
        repeat_values (keyboard, &seat->repeat_rate, &seat->repeat_delay);
        state = &keyboard->key->xkbInfo->state;
        seat->modifiers =
            (struct Modifiers){ state->base_mods, state->latched_mods,
                                state->locked_mods, state->group };
    }
    seat->global = wl_global_create (compositor.wl_display, &wl_seat_interface,
                                     SEAT_VERSION, seat, bind_seat);
    if (!seat->global)
        goto fail;
    seat->had_capabilities = seat->capabilities;
    wl_list_insert (compositor.input->seats.prev, &seat->link);
    seat->pointer_root = root_for_window (GetSpriteWindow (pointer));
    if (seat->pointer_root)
    {
        WindowPtr window =
            mb_window_get (&seat->pointer_root->window, DixGetAttrAccess);

        if (window)
        {
            seat->root_pointer_x = pointer->spriteInfo->sprite->hot.x;
            seat->root_pointer_y = pointer->spriteInfo->sprite->hot.y;
            pointer_motion (seat, seat->pointer_root,
                            seat->root_pointer_x - window->drawable.x +
                                seat->pointer_root->scene_x,
                            seat->root_pointer_y - window->drawable.y +
                                seat->pointer_root->scene_y,
                            GetTimeInMillis (), FALSE);
        }
    }
    if (keyboard->focus && keyboard->key)
    {
        MbSurface *focus = keyboard->focus->win == PointerRootWin
                               ? seat->pointer_root
                               : root_for_window (keyboard->focus->win);

        keyboard_focus (seat, focus, keyboard->key->down);
    }
    return TRUE;

fail:
    if (seat->keyboard_subscription)
        FreeResource (seat->keyboard_subscription, RT_NONE);
    if (seat->keymap_fd != INVALID_KEYMAP_FD)
        close (seat->keymap_fd);
    free (seat);
    return FALSE;
}

static void
reconcile_seats (void)
{
    struct MbSeat *seat;
    DeviceIntPtr pointer, keyboard;

    wl_list_for_each (seat, &compositor.input->seats, link)
    {
        if (seat->retire_pending && !seat->removed)
            retire_seat (seat);
        seat->seen = FALSE;
    }
    for (pointer = inputInfo.devices; pointer; pointer = pointer->next)
    {
        if (pointer->type != MASTER_POINTER)
            continue;
        keyboard = GetMaster (pointer, MASTER_KEYBOARD);
        if (!keyboard)
            continue;
        seat = find_seat (pointer->id);
        if (seat && seat->keyboard_id != keyboard->id)
        {
            retire_seat (seat);
            seat = NULL;
        }
        if (seat)
            seat->seen = TRUE;
        else if (!create_seat (pointer, keyboard))
            mb_client_disconnect (compositor.bridge_client);
    }
    wl_list_for_each (seat, &compositor.input->seats, link)
    {
        if (!seat->removed && !seat->seen)
            retire_seat (seat);
    }
}

static MbSurface *
hit_surface (MbSurface *root, double x, double y, MbSurface *grab,
             wl_fixed_t *surface_x, wl_fixed_t *surface_y)
{
    MbSceneIterator iterator = { .root = root, .surface = root };
    MbSurface *surface, *hit = NULL;

    while ((surface = mb_scene_next (&iterator)))
    {
        double local_x = x - iterator.x;
        double local_y = y - iterator.y;

        if (grab ? surface != grab
                 : local_x < 0 || local_y < 0 || local_x >= surface->width ||
                       local_y >= surface->height ||
                       (!surface->input_default &&
                        !pixman_region32_contains_point (
                            &surface->input, local_x, local_y, NULL)))
            continue;
        hit = surface;
        *surface_x = wl_fixed_from_double (local_x);
        *surface_y = wl_fixed_from_double (local_y);
    }
    return hit;
}

static MbDragPosition
drag_position (MbSurface *root, ScreenPtr screen, double x, double y,
               uint32_t time)
{
    MbDragPosition position = {
        .screen = screen, .root_x = x, .root_y = y, .time = time
    };
    WindowPtr window =
        root ? mb_window_get (&root->window, DixGetAttrAccess) : NULL;

    if (window && window->realized && root->pixels)
        position.surface =
            hit_surface (root, x - window->drawable.x + root->scene_x,
                         y - window->drawable.y + root->scene_y, NULL,
                         &position.x, &position.y);
    return position;
}

Bool
mb_input_begin_drag (uint64_t id, MbSurface *origin, uint32_t serial,
                     MbDragPosition *position)
{
    struct MbSeat *seat = find_seat_id (id);
    DeviceIntPtr pointer;
    GrabPtr grab;
    unsigned char events[XIMaskLen (XI_LASTEVENT)] = { 0 };
    unsigned char empty[sizeof (events)] = { 0 };
    unsigned char *masks[XIAllMasterDevices + 1] = { [XIAllDevices] = empty,
                                                     [XIAllMasterDevices] =
                                                         events };
    XI2Mask xi2mask = { .masks = masks,
                        .nmasks = ARRAY_SIZE (masks),
                        .mask_size = sizeof (events) };
    GrabMask mask = { .xi2mask = &xi2mask };
    CARD8 status;
    MbSurface *root;

    if (!seat || seat->removed || seat->retire_pending || seat->drag_owner ||
        seat->grab_focus != origin || !seat->button_count ||
        serial != seat->button_serial ||
        dixLookupDevice (&pointer, seat->pointer_id, origin->client->native,
                         DixGrabAccess) != Success)
        return FALSE;
    grab = pointer->deviceGrab.grab;
    if (!grab || !pointer->deviceGrab.implicitGrab || grab->grabtype != XI2 ||
        CLIENT_ID (grab->resource) != origin->client->native->index)
        return FALSE;
    seat->drag_screen = pointer->spriteInfo->sprite->hotPhys.pScreen;
    XISetMask (events, XI_ButtonPress);
    XISetMask (events, XI_ButtonRelease);
    XISetMask (events, XI_Motion);
    if (GrabDevice (origin->client->native, pointer, GrabModeAsync,
                    GrabModeAsync, seat->drag_screen->root->drawable.id, FALSE,
                    CurrentTime, &mask, XI2, None, None, &status) != Success ||
        status != GrabSuccess)
        return FALSE;
    if (!pointer->button || !pointer->button->buttonsDown)
    {
        pointer->deviceGrab.DeactivateGrab (pointer);
        return FALSE;
    }
    seat->drag_owner = origin->client->lifetime;
    seat->drag_enter_serial = seat->enter_serial;
    seat->root_pointer_x = pointer->spriteInfo->sprite->hot.x;
    seat->root_pointer_y = pointer->spriteInfo->sprite->hot.y;
    pointer_focus (seat, NULL, 0, 0);
    seat->pointer_root = NULL;
    root = root_for_window (GetSpriteWindow (pointer));
    *position = drag_position (root, seat->drag_screen, seat->root_pointer_x,
                               seat->root_pointer_y, GetTimeInMillis ());
    return TRUE;
}

void
mb_input_end_drag (uint64_t id)
{
    struct MbSeat *seat = find_seat_id (id);
    MbClient *client;
    DeviceIntPtr pointer;

    if (!seat || !seat->drag_owner)
        return;
    client = mb_client_lookup (seat->drag_owner);
    seat->drag_owner = 0;
    if (client &&
        dixLookupDevice (&pointer, seat->pointer_id, client->native,
                         DixGrabAccess) == Success &&
        pointer->deviceGrab.grab &&
        CLIENT_ID (pointer->deviceGrab.grab->resource) ==
            client->native->index)
        pointer->deviceGrab.DeactivateGrab (pointer);
    seat->drag_screen = NULL;
    seat->grab_focus = NULL;
    seat->button_count = 0;
    memset (seat->buttons, 0, sizeof (seat->buttons));
    clear_cursor (seat);
}

static void
pointer_motion (struct MbSeat *seat, MbSurface *root, double x, double y,
                uint32_t time, Bool motion)
{
    struct InputResource *input;
    MbSurface *surface;
    wl_fixed_t sx = 0, sy = 0;

    surface = root ? hit_surface (root, x, y,
                                  seat->button_count ? seat->grab_focus : NULL,
                                  &sx, &sy)
                   : NULL;
    if (seat->button_count && !surface)
    {
        seat->grab_focus = NULL;
        seat->button_count = 0;
        memset (seat->buttons, 0, sizeof (seat->buttons));
    }
    if (seat->pointer_focus != surface)
        pointer_focus (seat, surface, sx, sy);
    else if (surface &&
             (motion || seat->pointer_x != sx || seat->pointer_y != sy))
    {
        seat->pointer_x = sx;
        seat->pointer_y = sy;
        wl_list_for_each (input, &seat->resources, link)
        {
            if (input->kind == RESOURCE_POINTER &&
                input->client == surface->client && !input->client->failed)
            {
                wl_pointer_send_motion (input->resource, time, sx, sy);
                send_pointer_frame (input);
            }
        }
    }
}

static void
pointer_button (struct MbSeat *seat, unsigned int detail, Bool pressed,
                uint32_t time)
{
    struct InputResource *input;
    uint32_t serial, button;
    Bool wheel = detail >= X11_SCROLL_UP && detail <= X11_SCROLL_RIGHT;

    if (!seat->pointer_focus || detail >= KEYCODE_COUNT || !detail)
        return;
    if (wheel && !pressed)
        return;
    serial = wl_display_next_serial (compositor.wl_display);
    if (!wheel)
    {
        if (pressed == !!BitIsOn (seat->buttons, detail))
            return;
        if (pressed)
        {
            SetBit (seat->buttons, detail);
            if (!seat->button_count++)
                seat->grab_focus = seat->pointer_focus;
            seat->button_serial = serial;
            seat->grab_button = detail;
        }
        else
        {
            ClearBit (seat->buttons, detail);
            if (seat->button_count)
                seat->button_count--;
        }
    }
    button = detail == X11_LEFT_BUTTON     ? BTN_LEFT
             : detail == X11_MIDDLE_BUTTON ? BTN_MIDDLE
             : detail == X11_RIGHT_BUTTON
                 ? BTN_RIGHT
                 : BTN_SIDE + detail - X11_SIDE_BUTTON;
    wl_list_for_each (input, &seat->resources, link)
    {
        if (input->kind != RESOURCE_POINTER ||
            input->client != seat->pointer_focus->client ||
            input->client->failed)
            continue;
        if (wheel)
        {
            uint32_t axis = detail <= X11_SCROLL_DOWN
                                ? WL_POINTER_AXIS_VERTICAL_SCROLL
                                : WL_POINTER_AXIS_HORIZONTAL_SCROLL;
            int direction =
                detail == X11_SCROLL_UP || detail == X11_SCROLL_LEFT ? -1 : 1;

            if (wl_resource_get_version (input->resource) >=
                WL_POINTER_AXIS_SOURCE_SINCE_VERSION)
            {
                wl_pointer_send_axis_source (input->resource,
                                             WL_POINTER_AXIS_SOURCE_WHEEL);
                wl_pointer_send_axis_discrete (input->resource, axis,
                                               direction);
            }
            wl_pointer_send_axis (input->resource, time, axis,
                                  wl_fixed_from_int (direction * SCROLL_STEP));
        }
        else
        {
            record_serial (seat, input->client, serial);
            wl_pointer_send_button (input->resource, serial, time, button,
                                    pressed
                                        ? WL_POINTER_BUTTON_STATE_PRESSED
                                        : WL_POINTER_BUTTON_STATE_RELEASED);
        }
        send_pointer_frame (input);
    }
    if (!seat->button_count)
        seat->grab_focus = NULL;
}

static void
keyboard_key (struct MbSeat *seat, const xXIDeviceEvent *event)
{
    struct InputResource *input;
    unsigned int key = event->detail;
    Bool pressed = event->evtype == XI_KeyPress;
    uint32_t serial;

    if (seat->drag_owner && key == KEY_ESC + XORG_KEYCODE_OFFSET && pressed)
    {
        mb_data_drag_cancel (seat->lifetime);
        return;
    }
    if (!seat->keyboard_focus || key < XORG_KEYCODE_OFFSET ||
        key >= KEYCODE_COUNT || (event->flags & XIKeyRepeat))
        return;
    if (pressed == !!BitIsOn (seat->keys, key))
        return;
    if (pressed)
        SetBit (seat->keys, key);
    else
        ClearBit (seat->keys, key);
    serial = wl_display_next_serial (compositor.wl_display);
    wl_list_for_each (input, &seat->resources, link)
    {
        if (input->kind == RESOURCE_KEYBOARD &&
            input->client == seat->keyboard_focus->client &&
            !input->client->failed)
        {
            record_serial (seat, input->client, serial);
            wl_keyboard_send_key (input->resource, serial, event->time,
                                  key - XORG_KEYCODE_OFFSET,
                                  pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
                                          : WL_KEYBOARD_KEY_STATE_RELEASED);
        }
    }
}

static void
dispatch_xi (struct MbSeat *seat, struct InputEvent *queued)
{
    const xXIGenericDeviceEvent *generic = (const void *)queued->data;
    MbSurface *root;

    if (generic->evtype >= XI_Enter && generic->evtype <= XI_FocusOut)
    {
        const xXIEnterEvent *event = (const void *)generic;

        if (queued->size < sizeof (*event))
            return;
        root = window_surface (queued->client, event->event);
        if (!root)
            return;
        if (generic->evtype == XI_FocusIn)
        {
            WindowPtr window = mb_window_get (&root->window, DixGetAttrAccess);

            if (!window || !window->realized || !root->pixels)
                return;
            seat->modifiers = (struct Modifiers){
                event->mods.base_mods, event->mods.latched_mods,
                event->mods.locked_mods, event->group.effective_group
            };
            keyboard_focus (seat, root, queued->keys);
        }
        else if (generic->evtype == XI_FocusOut)
        {
            if (seat->keyboard_focus == root)
                keyboard_focus (seat, NULL, NULL);
        }
        else if (generic->evtype == XI_Leave)
        {
            if (!seat->button_count && seat->pointer_root == root)
            {
                seat->pointer_root = NULL;
                pointer_focus (seat, NULL, 0, 0);
            }
        }
        else
        {
            seat->pointer_root = root;
            seat->root_pointer_x = (double)event->root_x / FIXED_16_ONE;
            seat->root_pointer_y = (double)event->root_y / FIXED_16_ONE;
            pointer_motion (
                seat, root,
                (double)event->event_x / FIXED_16_ONE + root->scene_x,
                (double)event->event_y / FIXED_16_ONE + root->scene_y,
                event->time, FALSE);
        }
        return;
    }
    if (queued->size >= sizeof (xXIDeviceEvent))
    {
        const xXIDeviceEvent *event = (const void *)generic;

        if (seat->drag_owner && event->evtype >= XI_ButtonPress &&
            event->evtype <= XI_Motion)
        {
            MbDragPosition position;
            unsigned int detail = event->detail;

            if (!queued->drag_event || queued->client != seat->drag_owner)
                return;
            root =
                window_surface (queued->target_client, queued->target_window);
            seat->root_pointer_x = (double)event->root_x / FIXED_16_ONE;
            seat->root_pointer_y = (double)event->root_y / FIXED_16_ONE;
            if (queued->target_screen >= 0 &&
                queued->target_screen < screenInfo.numScreens)
                seat->drag_screen = screenInfo.screens[queued->target_screen];
            position =
                drag_position (root, seat->drag_screen, seat->root_pointer_x,
                               seat->root_pointer_y, event->time);
            mb_data_drag_motion (seat->lifetime, &position);
            if (!seat->drag_owner || event->evtype == XI_Motion || !detail ||
                detail >= KEYCODE_COUNT ||
                (detail >= X11_SCROLL_UP && detail <= X11_SCROLL_RIGHT))
                return;
            if (event->evtype == XI_ButtonPress &&
                !BitIsOn (seat->buttons, detail))
            {
                SetBit (seat->buttons, detail);
                seat->button_count++;
            }
            else if (event->evtype == XI_ButtonRelease &&
                     BitIsOn (seat->buttons, detail))
            {
                ClearBit (seat->buttons, detail);
                if (seat->button_count)
                    seat->button_count--;
                if (!seat->button_count)
                    mb_data_drag_drop (seat->lifetime);
            }
            return;
        }
        root = window_surface (queued->client, event->event);
        if (!root)
            return;
        if (event->evtype == XI_KeyPress || event->evtype == XI_KeyRelease)
        {
            if (seat->keyboard_focus == root)
                keyboard_key (seat, event);
            return;
        }
        seat->pointer_root = root;
        seat->root_pointer_x = (double)event->root_x / FIXED_16_ONE;
        seat->root_pointer_y = (double)event->root_y / FIXED_16_ONE;
        seat->root_x = seat->root_pointer_x;
        seat->root_y = seat->root_pointer_y;
        pointer_motion (seat, root,
                        (double)event->event_x / FIXED_16_ONE + root->scene_x,
                        (double)event->event_y / FIXED_16_ONE + root->scene_y,
                        event->time, event->evtype == XI_Motion);
        if (event->evtype == XI_ButtonPress ||
            event->evtype == XI_ButtonRelease)
            pointer_button (seat, event->detail,
                            event->evtype == XI_ButtonPress, event->time);
    }
}

Bool
mb_input_dispatch (void)
{
    struct MbInput *input = compositor.input;
    struct MbSeat *seat;
    unsigned int dispatched = 0;

    if (!input)
        return FALSE;
    reconcile_seats ();
    while (!wl_list_empty (&input->events) &&
           dispatched++ < INPUT_DISPATCH_LIMIT)
    {
        struct InputEvent *event =
            wl_container_of (input->events.next, event, link);
        MbClient *client = mb_client_lookup (event->client);

        seat = NULL;
        wl_list_for_each (seat, &input->seats, link)
        {
            if (seat->lifetime == event->seat)
                break;
        }
        if (&seat->link == &input->seats || seat->removed || !client)
        {
            free_event (event);
            continue;
        }
        switch (event->kind)
        {
        case INPUT_XI:
            dispatch_xi (seat, event);
            break;
        case INPUT_STATE:
            modifiers_changed (seat, event->modifiers);
            break;
        case INPUT_CONTROLS:
            repeat_changed (seat, event->repeat_rate, event->repeat_delay);
            break;
        case INPUT_KEYMAP:
        {
            struct InputResource *resource;
            uint32_t serial = wl_display_next_serial (compositor.wl_display);

            if (seat->keymap_fd != INVALID_KEYMAP_FD)
                close (seat->keymap_fd);
            seat->keymap_fd = event->keymap_fd;
            event->keymap_fd = INVALID_KEYMAP_FD;
            seat->keymap_size = event->keymap_size;
            seat->modifiers = event->modifiers;
            wl_list_for_each (resource, &seat->resources, link)
            {
                if (resource->kind != RESOURCE_KEYBOARD ||
                    resource->client->failed)
                    continue;
                wl_keyboard_send_keymap (resource->resource,
                                         WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
                                         seat->keymap_fd, seat->keymap_size);
                if (seat->keyboard_focus &&
                    seat->keyboard_focus->client == resource->client)
                    wl_keyboard_send_modifiers (
                        resource->resource, serial, seat->modifiers.depressed,
                        seat->modifiers.latched, seat->modifiers.locked,
                        seat->modifiers.group);
            }
            repeat_changed (seat, event->repeat_rate, event->repeat_delay);
            break;
        }
        }
        free_event (event);
    }
    wl_list_for_each (seat, &input->seats, link)
    {
        MbSurface *root = seat->pointer_root;
        WindowPtr window;

        if (seat->removed)
            continue;
        if (seat->drag_owner)
        {
            MbClient *owner = mb_client_lookup (seat->drag_owner);
            DeviceIntPtr pointer;

            if (!owner ||
                dixLookupDevice (&pointer, seat->pointer_id, owner->native,
                                 DixGetAttrAccess) != Success ||
                !pointer->deviceGrab.grab ||
                CLIENT_ID (pointer->deviceGrab.grab->resource) !=
                    owner->native->index)
                mb_data_drag_cancel (seat->lifetime);
            else
            {
                MbDragPosition position;

                root = root_for_window (GetSpriteWindow (pointer));
                seat->drag_screen =
                    pointer->spriteInfo->sprite->hotPhys.pScreen;
                seat->root_pointer_x = pointer->spriteInfo->sprite->hot.x;
                seat->root_pointer_y = pointer->spriteInfo->sprite->hot.y;
                position = drag_position (
                    root, seat->drag_screen, seat->root_pointer_x,
                    seat->root_pointer_y, GetTimeInMillis ());
                mb_data_drag_motion (seat->lifetime, &position);
            }
            root = NULL;
        }
        if (root)
        {
            window = mb_window_get (&root->window, DixGetAttrAccess);
            pointer_motion (seat, window && window->realized ? root : NULL,
                            window ? seat->root_pointer_x -
                                         window->drawable.x + root->scene_x
                                   : 0,
                            window ? seat->root_pointer_y -
                                         window->drawable.y + root->scene_y
                                   : 0,
                            GetTimeInMillis (), FALSE);
        }
        if (seat->keyboard_focus)
        {
            window = mb_window_get (&seat->keyboard_focus->window,
                                    DixGetAttrAccess);
            if (!window || !window->realized || !seat->keyboard_focus->pixels)
                keyboard_focus (seat, NULL, NULL);
        }
    }
    return !wl_list_empty (&input->events);
}

Bool
mb_input_select_window (MbWindow *window, MbClient *client)
{
    DeviceIntRec all_masters = { .id = XIAllMasterDevices };
    unsigned char mask[XIMaskLen (XI_LASTEVENT)] = { 0 };
    WindowPtr native;

    if (!compositor.input)
        return TRUE;
    native = mb_window_get (window, DixReceiveAccess);
    if (!native)
        return FALSE;
    XISetMask (mask, XI_KeyPress);
    XISetMask (mask, XI_KeyRelease);
    XISetMask (mask, XI_ButtonPress);
    XISetMask (mask, XI_ButtonRelease);
    XISetMask (mask, XI_Motion);
    XISetMask (mask, XI_Enter);
    XISetMask (mask, XI_Leave);
    XISetMask (mask, XI_FocusIn);
    XISetMask (mask, XI_FocusOut);
    return XISetEventMask (&all_masters, native, client->native, sizeof (mask),
                           mask) == Success;
}

void
mb_input_surface_destroyed (MbSurface *surface)
{
    struct MbSeat *seat;

    if (!compositor.input)
        return;
    wl_list_for_each (seat, &compositor.input->seats, link)
    {
        if (seat->cursor_surface == surface)
            clear_cursor (seat);
        if (seat->pointer_root == surface || seat->pointer_focus == surface ||
            seat->grab_focus == surface)
        {
            pointer_focus (seat, NULL, 0, 0);
            seat->grab_focus = NULL;
            if (seat->pointer_root == surface)
                seat->pointer_root = NULL;
            seat->button_count = 0;
            memset (seat->buttons, 0, sizeof (seat->buttons));
        }
        if (seat->keyboard_focus == surface)
            keyboard_focus (seat, NULL, NULL);
    }
}

Bool
mb_input_initialize (void)
{
    struct MbInput *input;
    ExtensionEntry *extension = CheckExtension ("XInputExtension");
    DeviceIntRec all_devices = { .id = XIAllDevices };
    unsigned char mask[XIMaskLen (XI_LASTEVENT)] = { 0 };
    int screen;

    if (!extension || !RT_XKBCLIENT)
        return FALSE;
    input = calloc (1, sizeof (*input));
    if (!input)
        return FALSE;
    wl_list_init (&input->seats);
    wl_list_init (&input->events);
    input->xi_opcode = extension->base;
    compositor.input = input;
    compositor.bridge_client->native->xkbClientFlags |= _XkbClientInitialized;
    compositor.bridge_client->native->mapNotifyMask = XkbAllMapComponentsMask;
    compositor.bridge_client->native->newKeyboardNotifyMask =
        XkbAllNewKeyboardEventsMask;
    if (!AddCallback (&EventCallback, native_event, NULL))
        goto fail;
    input->callback_registered = TRUE;
    XISetMask (mask, XI_HierarchyChanged);
    XISetMask (mask, XI_DeviceChanged);
    for (screen = 0; screen < screenInfo.numScreens; screen++)
    {
        WindowPtr root;

        if (dixLookupWindow (&root,
                             screenInfo.screens[screen]->root->drawable.id,
                             compositor.bridge_client->native,
                             DixReceiveAccess) != Success ||
            XISetEventMask (&all_devices, root,
                            compositor.bridge_client->native, sizeof (mask),
                            mask) != Success)
            goto fail;
    }
    reconcile_seats ();
    if (compositor.bridge_client->failed)
        goto fail;
    return TRUE;

fail:
    mb_input_shutdown ();
    return FALSE;
}

void
mb_input_shutdown (void)
{
    struct MbInput *input = compositor.input;
    struct MbSeat *seat, *next_seat;
    struct InputEvent *event, *next_event;

    if (!input)
        return;
    if (input->callback_registered)
        DeleteCallback (&EventCallback, native_event, NULL);
    wl_list_for_each_safe (event, next_event, &input->events, link)
    {
        free_event (event);
    }
    compositor.input = NULL;
    wl_list_for_each_safe (seat, next_seat, &input->seats, link)
    {
        struct InputResource *resource, *next_resource;

        clear_cursor (seat);
        wl_global_destroy (seat->global);
        wl_list_for_each_safe (resource, next_resource, &seat->resources, link)
        {
            wl_resource_destroy (resource->resource);
        }
        if (seat->keyboard_subscription && compositor.bridge_client->native)
            FreeResource (seat->keyboard_subscription, RT_NONE);
        if (seat->keymap_fd != INVALID_KEYMAP_FD)
            close (seat->keymap_fd);
        wl_list_remove (&seat->link);
        free (seat);
    }
    free (input);
}
