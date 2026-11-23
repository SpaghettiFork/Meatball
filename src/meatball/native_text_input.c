/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "server.h"

#include "input-method-v2.h"
#include "text-input-v3.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-protocol.h>
#include <window.h>

#define TEXT_PROTOCOL_VERSION 1
#define TEXT_MAX_BYTES 4000
#define UTF8_CONTINUATION_MASK 0xc0
#define UTF8_CONTINUATION_BITS 0x80

enum TextChanges
{
    TEXT_SURROUNDING = 1 << 0,
    TEXT_CONTENT = 1 << 1,
    TEXT_RECTANGLE = 1 << 2,
};

enum TextAction
{
    TEXT_KEEP,
    TEXT_ENABLE,
    TEXT_DISABLE,
};

struct TextRectangle
{
    int32_t x, y, width, height;
};

struct TextState
{
    char *surrounding;
    uint32_t cursor, anchor, cause, hint, purpose;
    struct TextRectangle rectangle;
    unsigned int changes;
};

struct TextInput
{
    struct wl_list link;
    struct wl_resource *resource;
    uint64_t seat;
    MbSurface *focus;
    struct TextState pending, current;
    enum TextAction action;
    uint32_t serial;
    Bool enabled;
};

struct MethodState
{
    char *preedit, *commit;
    int32_t begin, end;
    uint32_t before, after;
};

struct InputMethod
{
    struct wl_list link, popups;
    struct wl_resource *resource, *grab;
    struct TextInput *active;
    struct MethodState pending;
    uint64_t seat;
    uint32_t serial, activation_serial;
};

struct InputPopup
{
    struct wl_list link;
    struct wl_resource *resource;
    struct InputMethod *method;
    MbSurface *surface;
    struct TextRectangle rectangle;
    Bool rectangle_sent;
};

struct MbTextInput
{
    struct wl_global *text_global, *method_global;
    struct wl_list inputs, methods;
};

static struct InputMethod *
find_method (uint64_t seat)
{
    struct InputMethod *method;

    if (!seat || !compositor.text_input)
        return NULL;
    wl_list_for_each (method, &compositor.text_input->methods, link)
    {
        if (method->seat == seat)
            return method;
    }
    return NULL;
}

static void
clear_text_state (struct TextState *state)
{
    free (state->surrounding);
    *state = (struct TextState){ 0 };
}

static void
clear_method_state (struct InputMethod *method)
{
    free (method->pending.preedit);
    free (method->pending.commit);
    method->pending = (struct MethodState){ 0 };
}

static Bool
text_boundary (const char *text, size_t length, uint32_t offset)
{
    return offset <= length &&
           ((unsigned char)text[offset] & UTF8_CONTINUATION_MASK) !=
               UTF8_CONTINUATION_BITS;
}

static Bool
set_text (struct wl_resource *resource, char **target, const char *text)
{
    char *copy = strdup (text);

    if (!copy)
    {
        wl_resource_post_no_memory (resource);
        return FALSE;
    }
    free (*target);
    *target = copy;
    return TRUE;
}

static void
destroy_resource (struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_destroy (resource);
}

Bool
mb_text_input_popup_update (MbSurface *surface)
{
    struct InputPopup *popup;
    struct TextInput *input;
    MbSurface *root;
    WindowPtr window, parent;
    int64_t cursor_x, cursor_y, x, y;
    XID position[3];
    Mask changes = CWX | CWY;
    struct TextRectangle rectangle;

    if (!surface->input_popup)
        return TRUE;
    popup = wl_resource_get_user_data (surface->input_popup);
    window =
        mb_window_get (&surface->window, DixSetAttrAccess | DixManageAccess);
    if (!window)
        return TRUE;
    input = popup->method->active;
    root = input ? mb_subsurface_root (input->focus) : NULL;
    parent = root ? mb_window_get (&root->window, DixGetAttrAccess) : NULL;
    if (!parent || !parent->realized)
    {
        popup->rectangle_sent = FALSE;
        if (window->mapped)
            UnmapWindow (window, FALSE);
        return TRUE;
    }

    rectangle = input->current.rectangle;
    cursor_x = (int64_t)parent->drawable.x - root->scene_x + rectangle.x;
    cursor_y = (int64_t)parent->drawable.y - root->scene_y + rectangle.y;
    x = cursor_x;
    y = cursor_y + rectangle.height;
    if (y + window->drawable.height > window->drawable.pScreen->height)
        y = cursor_y - window->drawable.height;
    x = min (max (0, x), max (0, (int)window->drawable.pScreen->width -
                                     window->drawable.width));
    y = min (max (0, y), max (0, (int)window->drawable.pScreen->height -
                                     window->drawable.height));
    position[0] = x;
    position[1] = y;
    if (surface->content && !window->mapped)
    {
        changes |= CWStackMode;
        position[2] = Above;
    }
    if (((changes & CWStackMode) || x != window->drawable.x ||
         y != window->drawable.y) &&
        ConfigureWindow (window, changes, position, surface->client->native) !=
            Success)
        return FALSE;
    if (surface->content && !window->mapped &&
        MapWindow (window, surface->client->native) != Success)
        return FALSE;
    rectangle.x = min (INT32_MAX, max (INT32_MIN, cursor_x - x));
    rectangle.y = min (INT32_MAX, max (INT32_MIN, cursor_y - y));
    if (!popup->rectangle_sent ||
        memcmp (&rectangle, &popup->rectangle, sizeof (rectangle)))
    {
        zwp_input_popup_surface_v2_send_text_input_rectangle (
            popup->resource, rectangle.x, rectangle.y, rectangle.width,
            rectangle.height);
        popup->rectangle = rectangle;
        popup->rectangle_sent = TRUE;
    }
    return TRUE;
}

void
mb_text_input_window_changed (MbSurface *surface)
{
    struct InputMethod *method;
    struct InputPopup *popup;

    if (!compositor.text_input)
        return;
    wl_list_for_each (method, &compositor.text_input->methods, link)
    {
        if (!method->active ||
            mb_subsurface_root (method->active->focus) != surface)
            continue;
        wl_list_for_each (popup, &method->popups, link)
        {
            if (!mb_text_input_popup_update (popup->surface))
                wl_resource_post_no_memory (popup->resource);
        }
    }
}

static void
method_update (struct InputMethod *method, struct TextInput *input, Bool reset,
               unsigned int changes, uint32_t cause)
{
    struct InputPopup *popup;

    if (!method)
        return;
    reset |= method->active != input;
    if (reset && method->active)
        zwp_input_method_v2_send_deactivate (method->resource);
    method->active = input;
    if (reset)
    {
        clear_method_state (method);
        if (input)
        {
            zwp_input_method_v2_send_activate (method->resource);
            method->activation_serial = method->serial + 1;
            changes = input->current.changes;
        }
    }
    if (input)
    {
        const struct TextState *state = &input->current;

        zwp_input_method_v2_send_text_change_cause (method->resource, cause);
        if ((changes & TEXT_SURROUNDING) && state->surrounding)
            zwp_input_method_v2_send_surrounding_text (
                method->resource, state->surrounding, state->cursor,
                state->anchor);
        if (changes & TEXT_CONTENT)
            zwp_input_method_v2_send_content_type (
                method->resource, state->hint, state->purpose);
    }
    if (reset || input)
    {
        zwp_input_method_v2_send_done (method->resource);
        method->serial++;
    }
    if (reset || (changes & TEXT_RECTANGLE))
    {
        wl_list_for_each (popup, &method->popups, link)
        {
            if (!mb_text_input_popup_update (popup->surface))
                wl_resource_post_no_memory (popup->resource);
        }
    }
}

static void
text_focus (struct TextInput *input, MbSurface *surface)
{
    if (input->focus == surface)
        return;
    if (input->focus)
    {
        if (input->enabled)
            method_update (find_method (input->seat), NULL, FALSE, 0, 0);
        zwp_text_input_v3_send_leave (input->resource, input->focus->resource);
        input->enabled = FALSE;
        input->action = TEXT_KEEP;
        clear_text_state (&input->pending);
        clear_text_state (&input->current);
    }
    input->focus = surface;
    if (surface)
        zwp_text_input_v3_send_enter (input->resource, surface->resource);
}

void
mb_text_input_focus (uint64_t seat, MbSurface *surface)
{
    struct TextInput *input;

    if (!compositor.text_input)
        return;
    wl_list_for_each (input, &compositor.text_input->inputs, link)
    {
        if (input->seat == seat && input->focus != surface && input->focus)
            text_focus (input, NULL);
    }
    if (!surface)
        return;
    wl_list_for_each (input, &compositor.text_input->inputs, link)
    {
        if (input->seat == seat &&
            wl_resource_get_client (input->resource) ==
                wl_resource_get_client (surface->resource))
            text_focus (input, surface);
    }
}

static void
text_destroyed (struct wl_resource *resource)
{
    struct TextInput *input = wl_resource_get_user_data (resource);

    if (input->enabled)
        method_update (find_method (input->seat), NULL, FALSE, 0, 0);
    clear_text_state (&input->pending);
    clear_text_state (&input->current);
    wl_list_remove (&input->link);
    free (input);
}

static void
text_enable (struct wl_client *client, struct wl_resource *resource)
{
    struct TextInput *input = wl_resource_get_user_data (resource);

    if (input->focus)
    {
        clear_text_state (&input->pending);
        input->action = TEXT_ENABLE;
    }
}

static void
text_disable (struct wl_client *client, struct wl_resource *resource)
{
    struct TextInput *input = wl_resource_get_user_data (resource);

    if (input->focus)
    {
        clear_text_state (&input->pending);
        input->action = TEXT_DISABLE;
    }
}

static void
text_surrounding (struct wl_client *client, struct wl_resource *resource,
                  const char *text, int32_t cursor, int32_t anchor)
{
    struct TextInput *input = wl_resource_get_user_data (resource);
    size_t length = strlen (text);

    if (!input->focus || length > TEXT_MAX_BYTES || cursor < 0 || anchor < 0 ||
        !text_boundary (text, length, cursor) ||
        !text_boundary (text, length, anchor))
        return;
    if (!set_text (resource, &input->pending.surrounding, text))
        return;
    input->pending.cursor = cursor;
    input->pending.anchor = anchor;
    input->pending.changes |= TEXT_SURROUNDING;
}

static void
text_cause (struct wl_client *client, struct wl_resource *resource,
            uint32_t cause)
{
    struct TextInput *input = wl_resource_get_user_data (resource);

    if (input->focus && cause <= ZWP_TEXT_INPUT_V3_CHANGE_CAUSE_OTHER)
        input->pending.cause = cause;
}

static void
text_content (struct wl_client *client, struct wl_resource *resource,
              uint32_t hint, uint32_t purpose)
{
    struct TextInput *input = wl_resource_get_user_data (resource);

    if (input->focus && purpose <= ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_TERMINAL)
    {
        input->pending.hint = hint;
        input->pending.purpose = purpose;
        input->pending.changes |= TEXT_CONTENT;
    }
}

static void
text_rectangle (struct wl_client *client, struct wl_resource *resource,
                int32_t x, int32_t y, int32_t width, int32_t height)
{
    struct TextInput *input = wl_resource_get_user_data (resource);

    if (input->focus && width >= 0 && height >= 0)
    {
        input->pending.rectangle =
            (struct TextRectangle){ x, y, width, height };
        input->pending.changes |= TEXT_RECTANGLE;
    }
}

static void
text_commit (struct wl_client *client, struct wl_resource *resource)
{
    struct TextInput *input = wl_resource_get_user_data (resource), *other;
    struct TextState *pending = &input->pending, *current = &input->current;
    struct InputMethod *method;
    Bool reset = input->action == TEXT_ENABLE;

    input->serial++;
    if (!input->focus)
        return;
    method = find_method (input->seat);
    if (input->action == TEXT_DISABLE)
    {
        if (input->enabled)
            method_update (method, NULL, FALSE, 0, 0);
        input->enabled = FALSE;
        clear_text_state (current);
        goto clear;
    }
    if (reset)
    {
        wl_list_for_each (other, &compositor.text_input->inputs, link)
        {
            if (other != input && other->seat == input->seat && other->enabled)
                goto clear;
        }
        clear_text_state (current);
        input->enabled = TRUE;
    }
    if (!input->enabled)
        goto clear;
    if (pending->changes & TEXT_SURROUNDING)
    {
        free (current->surrounding);
        current->surrounding = pending->surrounding;
        pending->surrounding = NULL;
        current->cursor = pending->cursor;
        current->anchor = pending->anchor;
    }
    if (pending->changes & TEXT_CONTENT)
    {
        current->hint = pending->hint;
        current->purpose = pending->purpose;
    }
    if (pending->changes & TEXT_RECTANGLE)
        current->rectangle = pending->rectangle;
    current->changes |= pending->changes;
    method_update (method, input, reset, pending->changes, pending->cause);

clear:
    input->action = TEXT_KEEP;
    clear_text_state (pending);
}

static const struct zwp_text_input_v3_interface text_implementation = {
    .destroy = destroy_resource,
    .enable = text_enable,
    .disable = text_disable,
    .set_surrounding_text = text_surrounding,
    .set_text_change_cause = text_cause,
    .set_content_type = text_content,
    .set_cursor_rectangle = text_rectangle,
    .commit = text_commit,
};

static void
get_text_input (struct wl_client *client, struct wl_resource *resource,
                uint32_t id, struct wl_resource *seat)
{
    struct TextInput *input = calloc (1, sizeof (*input));
    MbSurface *focus;

    if (!input)
    {
        wl_client_post_no_memory (client);
        return;
    }
    input->resource = wl_resource_create (client, &zwp_text_input_v3_interface,
                                          TEXT_PROTOCOL_VERSION, id);
    if (!input->resource)
    {
        free (input);
        wl_client_post_no_memory (client);
        return;
    }
    input->seat = mb_input_seat_id (seat);
    wl_list_insert (&compositor.text_input->inputs, &input->link);
    wl_resource_set_implementation (input->resource, &text_implementation,
                                    input, text_destroyed);
    focus = mb_input_keyboard_surface (input->seat);
    if (focus && wl_resource_get_client (focus->resource) == client)
        text_focus (input, focus);
}

static const struct zwp_text_input_manager_v3_interface text_manager = {
    .destroy = destroy_resource,
    .get_text_input = get_text_input,
};

static void
method_commit_string (struct wl_client *client, struct wl_resource *resource,
                      const char *text)
{
    struct InputMethod *method = wl_resource_get_user_data (resource);

    if (method && method->active && strlen (text) <= TEXT_MAX_BYTES)
        set_text (resource, &method->pending.commit, text);
}

static void
method_preedit (struct wl_client *client, struct wl_resource *resource,
                const char *text, int32_t begin, int32_t end)
{
    struct InputMethod *method = wl_resource_get_user_data (resource);
    size_t length = strlen (text);

    if (!method || !method->active || length > TEXT_MAX_BYTES ||
        ((begin != -1 || end != -1) &&
         (begin < 0 || end < 0 || !text_boundary (text, length, begin) ||
          !text_boundary (text, length, end))))
        return;
    if (!set_text (resource, &method->pending.preedit, text))
        return;
    method->pending.begin = begin;
    method->pending.end = end;
}

static void
method_delete (struct wl_client *client, struct wl_resource *resource,
               uint32_t before, uint32_t after)
{
    struct InputMethod *method = wl_resource_get_user_data (resource);

    if (method && method->active)
    {
        method->pending.before = before;
        method->pending.after = after;
    }
}

static void
method_commit (struct wl_client *client, struct wl_resource *resource,
               uint32_t serial)
{
    struct InputMethod *method = wl_resource_get_user_data (resource);
    struct TextInput *input;
    struct MethodState *pending;

    if (!method)
        return;
    input = method->active;
    pending = &method->pending;
    /* A stale transaction can update the same field, but may not cross
     * an activation boundary and reach a different field or client. */
    if (input && (int32_t)(serial - method->activation_serial) >= 0 &&
        (int32_t)(method->serial - serial) >= 0)
    {
        const struct TextState *state = &input->current;

        if ((pending->before || pending->after) && state->surrounding)
        {
            size_t length = strlen (state->surrounding);
            if (pending->before <= state->cursor &&
                pending->after <= length - state->cursor &&
                text_boundary (state->surrounding, length,
                               state->cursor - pending->before) &&
                text_boundary (state->surrounding, length,
                               state->cursor + pending->after))
                zwp_text_input_v3_send_delete_surrounding_text (
                    input->resource, pending->before, pending->after);
        }
        if (pending->commit)
            zwp_text_input_v3_send_commit_string (input->resource,
                                                  pending->commit);
        zwp_text_input_v3_send_preedit_string (
            input->resource, pending->preedit, pending->begin, pending->end);
        zwp_text_input_v3_send_done (input->resource, input->serial);
    }
    clear_method_state (method);
}

static void
popup_destroyed (struct wl_resource *resource)
{
    struct InputPopup *popup = wl_resource_get_user_data (resource);
    WindowPtr window;

    if (!popup)
        return;
    popup->surface->input_popup = NULL;
    window = mb_window_get (&popup->surface->window, DixSetAttrAccess);
    if (window && window->mapped)
        UnmapWindow (window, FALSE);
    wl_list_remove (&popup->link);
    free (popup);
}

void
mb_text_input_popup_destroyed (MbSurface *surface)
{
    if (surface->input_popup)
        wl_resource_destroy (surface->input_popup);
}

static const struct zwp_input_popup_surface_v2_interface
    popup_implementation = {
        .destroy = destroy_resource,
    };

static void
get_popup (struct wl_client *client, struct wl_resource *resource, uint32_t id,
           struct wl_resource *surface_resource)
{
    struct InputMethod *method = wl_resource_get_user_data (resource);
    MbSurface *surface = wl_resource_get_user_data (surface_resource);
    struct wl_resource *child;
    struct InputPopup *popup;
    WindowPtr window;
    XID override = TRUE;

    if (method && method->seat && surface->role != MB_ROLE_NONE)
    {
        wl_resource_post_error (resource, ZWP_INPUT_METHOD_V2_ERROR_ROLE,
                                "input popup surface already has a role");
        return;
    }
    child = wl_resource_create (client, &zwp_input_popup_surface_v2_interface,
                                TEXT_PROTOCOL_VERSION, id);
    if (!child)
    {
        wl_client_post_no_memory (client);
        return;
    }
    wl_resource_set_implementation (child, &popup_implementation, NULL,
                                    popup_destroyed);
    if (!method || !method->seat)
        return;

    popup = calloc (1, sizeof (*popup));
    window = mb_window_get (&surface->window, DixSetAttrAccess);
    if (!popup || !window ||
        ChangeWindowAttributes (window, CWOverrideRedirect, &override,
                                serverClient) != Success)
    {
        free (popup);
        wl_resource_post_no_memory (resource);
        return;
    }
    popup->method = method;
    popup->surface = surface;
    popup->resource = child;
    surface->role = MB_ROLE_INPUT_POPUP;
    surface->input_popup = child;
    wl_list_insert (&method->popups, &popup->link);
    wl_resource_set_user_data (child, popup);
    if (surface->content)
        mb_surface_update (surface);
    else if (!mb_text_input_popup_update (surface))
        wl_resource_post_no_memory (child);
}

static void
grab_destroyed (struct wl_resource *resource)
{
    struct InputMethod *method = wl_resource_get_user_data (resource);

    if (method)
        method->grab = NULL;
}

static const struct zwp_input_method_keyboard_grab_v2_interface
    grab_implementation = {
        .release = destroy_resource,
    };

void
mb_text_input_modifiers (uint64_t seat, uint32_t serial, uint32_t depressed,
                         uint32_t latched, uint32_t locked, uint32_t group)
{
    struct InputMethod *method = find_method (seat);

    if (method && method->grab)
        zwp_input_method_keyboard_grab_v2_send_modifiers (
            method->grab, serial, depressed, latched, locked, group);
}

void
mb_text_input_repeat (uint64_t seat, int32_t rate, int32_t delay)
{
    struct InputMethod *method = find_method (seat);

    if (method && method->grab)
        zwp_input_method_keyboard_grab_v2_send_repeat_info (method->grab, rate,
                                                            delay);
}

void
mb_text_input_keyboard (uint64_t seat, const MbTextKeyboard *state)
{
    struct InputMethod *method = find_method (seat);

    if (!method || !method->grab)
        return;
    zwp_input_method_keyboard_grab_v2_send_repeat_info (
        method->grab, state->rate, state->delay);
    if (state->fd >= 0)
        zwp_input_method_keyboard_grab_v2_send_keymap (
            method->grab, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, state->fd,
            state->size);
    mb_text_input_modifiers (
        seat, wl_display_next_serial (compositor.wl_display), state->depressed,
        state->latched, state->locked, state->group);
}

Bool
mb_text_input_key (uint64_t seat, uint32_t serial, uint32_t time, uint32_t key,
                   uint32_t state)
{
    struct InputMethod *method = find_method (seat);

    if (!method || !method->grab ||
        (!method->active && state != WL_KEYBOARD_KEY_STATE_RELEASED))
        return FALSE;
    zwp_input_method_keyboard_grab_v2_send_key (method->grab, serial, time,
                                                key, state);
    return TRUE;
}

static void
method_grab (struct wl_client *client, struct wl_resource *resource,
             uint32_t id)
{
    struct InputMethod *method = wl_resource_get_user_data (resource);
    struct wl_resource *grab = wl_resource_create (
        client, &zwp_input_method_keyboard_grab_v2_interface,
        TEXT_PROTOCOL_VERSION, id);
    MbTextKeyboard keyboard;

    if (!grab)
    {
        wl_client_post_no_memory (client);
        return;
    }
    if (method && method->seat && !method->grab &&
        mb_input_text_keyboard (method->seat, &keyboard))
    {
        method->grab = grab;
        wl_resource_set_implementation (grab, &grab_implementation, method,
                                        grab_destroyed);
        mb_text_input_keyboard (method->seat, &keyboard);
    }
    else
        wl_resource_set_implementation (grab, &grab_implementation, NULL,
                                        NULL);
}

static void
method_destroyed (struct wl_resource *resource)
{
    struct InputMethod *method = wl_resource_get_user_data (resource);
    struct InputPopup *popup, *next;

    if (!method)
        return;
    if (method->active)
    {
        struct TextInput *input = method->active;

        zwp_text_input_v3_send_preedit_string (input->resource, NULL, 0, 0);
        zwp_text_input_v3_send_done (input->resource, input->serial);
    }
    wl_list_for_each_safe (popup, next, &method->popups, link)
        wl_resource_destroy (popup->resource);
    if (method->grab)
        wl_resource_destroy (method->grab);
    clear_method_state (method);
    wl_list_remove (&method->link);
    free (method);
}

static const struct zwp_input_method_v2_interface method_implementation = {
    .destroy = destroy_resource,
    .commit_string = method_commit_string,
    .set_preedit_string = method_preedit,
    .delete_surrounding_text = method_delete,
    .commit = method_commit,
    .get_input_popup_surface = get_popup,
    .grab_keyboard = method_grab,
};

static void
get_method (struct wl_client *client, struct wl_resource *resource,
            struct wl_resource *seat, uint32_t id)
{
    struct wl_resource *child = wl_resource_create (
        client, &zwp_input_method_v2_interface, TEXT_PROTOCOL_VERSION, id);
    uint64_t seat_id = mb_input_seat_id (seat);
    struct InputMethod *method;
    struct TextInput *input;
    MbTextKeyboard keyboard;

    if (!child)
    {
        wl_client_post_no_memory (client);
        return;
    }
    wl_resource_set_implementation (child, &method_implementation, NULL,
                                    method_destroyed);
    if (!mb_input_text_keyboard (seat_id, &keyboard) || find_method (seat_id))
    {
        zwp_input_method_v2_send_unavailable (child);
        return;
    }
    method = calloc (1, sizeof (*method));
    if (!method)
    {
        wl_client_post_no_memory (client);
        return;
    }
    method->resource = child;
    method->seat = seat_id;
    wl_list_init (&method->popups);
    wl_list_insert (&compositor.text_input->methods, &method->link);
    wl_resource_set_user_data (child, method);
    wl_list_for_each (input, &compositor.text_input->inputs, link)
    {
        if (input->seat == seat_id && input->enabled)
        {
            method_update (method, input, FALSE, 0, 0);
            break;
        }
    }
}

static const struct zwp_input_method_manager_v2_interface method_manager = {
    .destroy = destroy_resource,
    .get_input_method = get_method,
};

void
mb_text_input_seat_removed (uint64_t seat)
{
    struct InputMethod *method = find_method (seat);
    struct TextInput *input;

    if (!compositor.text_input)
        return;
    if (method)
    {
        method_update (method, NULL, FALSE, 0, 0);
        zwp_input_method_v2_send_unavailable (method->resource);
        method->seat = 0;
    }
    wl_list_for_each (input, &compositor.text_input->inputs, link)
    {
        if (input->seat == seat)
            input->seat = 0;
    }
}

static void
bind_manager (struct wl_client *client, void *data, uint32_t version,
              uint32_t id)
{
    struct wl_resource *resource;
    Bool text = data == &text_manager;

    resource =
        wl_resource_create (client,
                            text ? &zwp_text_input_manager_v3_interface
                                 : &zwp_input_method_manager_v2_interface,
                            version, id);
    if (!resource)
    {
        wl_client_post_no_memory (client);
        return;
    }
    wl_resource_set_implementation (resource, data, NULL, NULL);
}

static bool
allow_global (const struct wl_client *client, const struct wl_global *global,
              void *data)
{
    uid_t uid;

    if (wl_global_get_interface (global) !=
        &zwp_input_method_manager_v2_interface)
        return true;
    wl_client_get_credentials ((struct wl_client *)client, NULL, &uid, NULL);
    return uid == compositor.socket_uid;
}

Bool
mb_text_input_initialize (void)
{
    struct MbTextInput *state = calloc (1, sizeof (*state));

    if (!state)
        return FALSE;
    compositor.text_input = state;
    wl_list_init (&state->inputs);
    wl_list_init (&state->methods);
    wl_display_set_global_filter (compositor.wl_display, allow_global, NULL);
    state->text_global = wl_global_create (
        compositor.wl_display, &zwp_text_input_manager_v3_interface,
        TEXT_PROTOCOL_VERSION, (void *)&text_manager, bind_manager);
    state->method_global = wl_global_create (
        compositor.wl_display, &zwp_input_method_manager_v2_interface,
        TEXT_PROTOCOL_VERSION, (void *)&method_manager, bind_manager);
    return state->text_global && state->method_global;
}

void
mb_text_input_shutdown (void)
{
    struct MbTextInput *state = compositor.text_input;
    struct TextInput *input, *next_input;
    struct InputMethod *method, *next_method;

    if (!state)
        return;
    wl_list_for_each_safe (input, next_input, &state->inputs, link)
        wl_resource_destroy (input->resource);
    wl_list_for_each_safe (method, next_method, &state->methods, link)
        wl_resource_destroy (method->resource);
    if (state->text_global)
        wl_global_destroy (state->text_global);
    if (state->method_global)
        wl_global_destroy (state->method_global);
    wl_display_set_global_filter (compositor.wl_display, NULL, NULL);
    free (state);
    compositor.text_input = NULL;
}
