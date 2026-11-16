/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "server.h"

#include <randrstr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-protocol.h>

#define OUTPUT_VERSION 4
#define OUTPUT_SCALE 1
#define OUTPUT_LABEL_SIZE 256
#define OUTPUT_NAME_SIZE 64
#define OUTPUT_MAKE "Spaghetti"
#define OUTPUT_NAME_FORMAT "X11-%d-%x"
#define OUTPUT_DESCRIPTION_FORMAT "Spaghetti X11 screen %d: %s"
#define SCREEN_LABEL "X11 screen"
#define MILLIHERTZ_PER_HERTZ 1000
#define ROTATION_COUNT 4
#define ASCII_FIRST_PRINTABLE 0x20
#define ASCII_LAST_PRINTABLE 0x7e

struct OutputState
{
    int32_t x, y, width, height, mm_width, mm_height;
    int32_t mode_width, mode_height, refresh, subpixel, transform;
    uint32_t flags;
    char model[OUTPUT_LABEL_SIZE];
};

struct Output
{
    struct wl_list link, resources;
    struct wl_global *global;
    ScreenPtr screen;
    RROutput id;
    Bool seen, removed;
    char name[OUTPUT_NAME_SIZE];
    struct OutputState state;
};

struct OutputResource
{
    struct wl_list link, surfaces;
    struct wl_resource *resource;
    struct Output *output;
};

struct SurfaceOutput
{
    struct wl_list link;
    MbSurface *surface;
    Bool seen;
};

struct MbOutputs
{
    struct wl_list outputs;
};

static void refresh_surfaces (void);

static void
release (struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_destroy (resource);
}

static const struct wl_output_interface implementation = {
    .release = release,
};

static void
clear_memberships (struct OutputResource *binding, Bool notify)
{
    struct SurfaceOutput *membership, *next;

    wl_list_for_each_safe (membership, next, &binding->surfaces, link)
    {
        if (notify && !membership->surface->client->failed)
            wl_surface_send_leave (membership->surface->resource,
                                   binding->resource);
        wl_list_remove (&membership->link);
        free (membership);
    }
}

static void
resource_destroyed (struct wl_resource *resource)
{
    struct OutputResource *binding = wl_resource_get_user_data (resource);

    clear_memberships (binding, FALSE);
    wl_list_remove (&binding->link);
    free (binding);
}

static void
send_state (struct OutputResource *binding, Bool initial)
{
    struct Output *output = binding->output;
    struct OutputState *state = &output->state;
    struct wl_resource *resource = binding->resource;
    uint32_t version = wl_resource_get_version (resource);
    char description[OUTPUT_LABEL_SIZE + OUTPUT_NAME_SIZE];

    wl_output_send_geometry (resource, state->x, state->y, state->mm_width,
                             state->mm_height, state->subpixel, OUTPUT_MAKE,
                             state->model, state->transform);
    wl_output_send_mode (resource, state->flags, state->mode_width,
                         state->mode_height, state->refresh);
    if (initial && version >= WL_OUTPUT_SCALE_SINCE_VERSION)
        wl_output_send_scale (resource, OUTPUT_SCALE);
    if (initial && version >= WL_OUTPUT_NAME_SINCE_VERSION)
        wl_output_send_name (resource, output->name);
    if (version >= WL_OUTPUT_DESCRIPTION_SINCE_VERSION)
    {
        snprintf (description, sizeof (description), OUTPUT_DESCRIPTION_FORMAT,
                  output->screen->myNum, state->model);
        wl_output_send_description (resource, description);
    }
    if (version >= WL_OUTPUT_DONE_SINCE_VERSION)
        wl_output_send_done (resource);
}

static void
bind_output (struct wl_client *client, void *data, uint32_t version,
             uint32_t id)
{
    struct Output *output = data;
    struct OutputResource *binding = calloc (1, sizeof (*binding));

    if (!binding)
    {
        wl_client_post_no_memory (client);
        return;
    }
    binding->resource =
        wl_resource_create (client, &wl_output_interface, version, id);
    if (!binding->resource)
    {
        free (binding);
        wl_client_post_no_memory (client);
        return;
    }
    binding->output = output;
    wl_list_init (&binding->surfaces);
    wl_list_insert (output->resources.prev, &binding->link);
    wl_resource_set_implementation (binding->resource, &implementation,
                                    binding, resource_destroyed);
    send_state (binding, TRUE);
    refresh_surfaces ();
}

static int32_t
mode_refresh (const xRRModeInfo *mode)
{
    uint64_t numerator = (uint64_t)mode->dotClock * MILLIHERTZ_PER_HERTZ;
    uint64_t denominator = (uint64_t)mode->hTotal * mode->vTotal;

    if (!denominator)
        return 0;
    if (mode->modeFlags & RR_Interlace)
        numerator *= 2;
    if (mode->modeFlags & RR_DoubleScan)
        denominator *= 2;
    return min (INT32_MAX, (numerator + denominator / 2) / denominator);
}

static int32_t
output_transform (Rotation rotation)
{
    int32_t transform = WL_OUTPUT_TRANSFORM_NORMAL;
    Bool reflected =
        !!(rotation & RR_Reflect_X) != !!(rotation & RR_Reflect_Y);

    if (rotation & RR_Rotate_90)
        transform = WL_OUTPUT_TRANSFORM_90;
    else if (rotation & RR_Rotate_180)
        transform = WL_OUTPUT_TRANSFORM_180;
    else if (rotation & RR_Rotate_270)
        transform = WL_OUTPUT_TRANSFORM_270;
    /* RandR reflects after rotation; Wayland flips before rotation. */
    if (reflected)
        transform = (ROTATION_COUNT - transform) % ROTATION_COUNT;
    if (rotation & RR_Reflect_Y)
        transform = (transform + WL_OUTPUT_TRANSFORM_180) % ROTATION_COUNT;
    if (reflected)
        transform += WL_OUTPUT_TRANSFORM_FLIPPED;
    return transform;
}

static int32_t
output_subpixel (int order)
{
    switch (order)
    {
    case SubPixelHorizontalRGB:
        return WL_OUTPUT_SUBPIXEL_HORIZONTAL_RGB;
    case SubPixelHorizontalBGR:
        return WL_OUTPUT_SUBPIXEL_HORIZONTAL_BGR;
    case SubPixelVerticalRGB:
        return WL_OUTPUT_SUBPIXEL_VERTICAL_RGB;
    case SubPixelVerticalBGR:
        return WL_OUTPUT_SUBPIXEL_VERTICAL_BGR;
    case SubPixelNone:
        return WL_OUTPUT_SUBPIXEL_NONE;
    default:
        return WL_OUTPUT_SUBPIXEL_UNKNOWN;
    }
}

static Bool
update_output (ScreenPtr screen, RROutputPtr native)
{
    struct Output *output;
    struct OutputState state = { 0 };
    struct OutputResource *binding;
    RROutput id = native ? native->id : None;
    size_t i;

    state.x = screen->x;
    state.y = screen->y;
    if (native)
    {
        RRCrtcPtr crtc = native->crtc;
        RRModePtr mode = crtc->mode;

        state.x += crtc->x;
        state.y += crtc->y;
        RRCrtcGetScanoutSize (crtc, &state.width, &state.height);
        state.mm_width = max (0, native->mmWidth);
        state.mm_height = max (0, native->mmHeight);
        state.transform = output_transform (crtc->rotation);
        state.subpixel = output_subpixel (native->subpixelOrder);
        state.mode_width = mode->mode.width;
        state.mode_height = mode->mode.height;
        state.refresh = mode_refresh (&mode->mode);
        state.flags = WL_OUTPUT_MODE_CURRENT;
        for (i = 0; i < native->numPreferred; i++)
            if (native->modes[i] == mode)
                state.flags |= WL_OUTPUT_MODE_PREFERRED;
        for (i = 0; i < min (native->nameLength, sizeof (state.model) - 1);
             i++)
        {
            unsigned char byte = native->name[i];

            state.model[i] =
                byte >= ASCII_FIRST_PRINTABLE && byte <= ASCII_LAST_PRINTABLE
                    ? byte
                    : '?';
        }
    }
    else
    {
        state.width = state.mode_width = screen->width;
        state.height = state.mode_height = screen->height;
        state.mm_width = max (0, screen->mmWidth);
        state.mm_height = max (0, screen->mmHeight);
        state.flags = WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED;
        snprintf (state.model, sizeof (state.model), "%s", SCREEN_LABEL);
    }
    wl_list_for_each (output, &compositor.outputs->outputs, link)
    {
        if (!output->removed && output->screen == screen && output->id == id)
            break;
    }
    if (&output->link == &compositor.outputs->outputs)
    {
        output = calloc (1, sizeof (*output));
        if (!output)
            return FALSE;
        output->screen = screen;
        output->id = id;
        output->state = state;
        wl_list_init (&output->resources);
        snprintf (output->name, sizeof (output->name), OUTPUT_NAME_FORMAT,
                  screen->myNum, id);
        output->global =
            wl_global_create (compositor.wl_display, &wl_output_interface,
                              OUTPUT_VERSION, output, bind_output);
        if (!output->global)
        {
            free (output);
            return FALSE;
        }
        wl_list_insert (compositor.outputs->outputs.prev, &output->link);
    }
    output->seen = TRUE;
    if (memcmp (&output->state, &state, sizeof (state)))
    {
        output->state = state;
        wl_list_for_each (binding, &output->resources, link)
        {
            send_state (binding, FALSE);
        }
    }
    return TRUE;
}

static Bool
surface_enter (MbSurface *surface, struct OutputResource *binding)
{
    struct SurfaceOutput *membership;

    wl_list_for_each (membership, &binding->surfaces, link)
    {
        if (membership->surface == surface)
        {
            membership->seen = TRUE;
            return TRUE;
        }
    }
    membership = calloc (1, sizeof (*membership));
    if (!membership)
        return FALSE;
    membership->surface = surface;
    membership->seen = TRUE;
    wl_list_insert (binding->surfaces.prev, &membership->link);
    wl_surface_send_enter (surface->resource, binding->resource);
    return TRUE;
}

static void
refresh_surfaces (void)
{
    struct Output *output;
    struct OutputResource *binding;
    struct SurfaceOutput *membership, *next;
    MbSurface *root;

    wl_list_for_each (output, &compositor.outputs->outputs, link)
    {
        wl_list_for_each (binding, &output->resources, link)
        {
            wl_list_for_each (membership, &binding->surfaces, link)
            {
                membership->seen = FALSE;
            }
        }
    }
    wl_list_for_each (root, &compositor.surfaces, link)
    {
        MbSceneIterator iterator = { .root = root, .surface = root };
        MbSurface *surface;
        WindowPtr window;
        Bool cursor = root->role == MB_ROLE_CURSOR;

        if (!root->pixels || root->client->failed ||
            mb_subsurface_root (root) != root)
            continue;
        window = mb_window_get (&root->window, DixGetAttrAccess);
        if (!window ||
            (cursor ? !mb_input_cursor_visible (root) : !window->realized))
            continue;
        while ((surface = mb_scene_next (&iterator)))
        {
            pixman_box32_t node = { iterator.x, iterator.y,
                                    iterator.x + surface->width,
                                    iterator.y + surface->height };

            if (!cursor)
            {
                node.x1 = max (node.x1, root->scene_x);
                node.y1 = max (node.y1, root->scene_y);
                node.x2 = min (node.x2,
                               root->scene_x + min (root->scene_width,
                                                    window->drawable.width));
                node.y2 = min (node.y2,
                               root->scene_y + min (root->scene_height,
                                                    window->drawable.height));
                if (node.x1 >= node.x2 || node.y1 >= node.y2)
                    continue;
                int32_t x = window->drawable.x + root->window.screen->x -
                            root->scene_x;
                int32_t y = window->drawable.y + root->window.screen->y -
                            root->scene_y;

                node.x1 += x;
                node.x2 += x;
                node.y1 += y;
                node.y2 += y;
            }
            wl_list_for_each (output, &compositor.outputs->outputs, link)
            {
                struct OutputState *state = &output->state;
                pixman_box32_t bounds = { state->x, state->y,
                                          state->x + state->width,
                                          state->y + state->height };

                if (output->removed ||
                    (!cursor && output->screen != root->window.screen))
                    continue;
                if (cursor ? !mb_input_cursor_intersects (root, output->screen,
                                                          &node, &bounds)
                           : node.x1 >= bounds.x2 || node.x2 <= bounds.x1 ||
                                 node.y1 >= bounds.y2 || node.y2 <= bounds.y1)
                    continue;
                wl_list_for_each (binding, &output->resources, link)
                {
                    if (wl_resource_get_client (binding->resource) ==
                            surface->client->wayland &&
                        !surface_enter (surface, binding))
                    {
                        wl_resource_post_no_memory (surface->resource);
                        return;
                    }
                }
            }
        }
    }
    wl_list_for_each (output, &compositor.outputs->outputs, link)
    {
        wl_list_for_each (binding, &output->resources, link)
        {
            wl_list_for_each_safe (membership, next, &binding->surfaces, link)
            {
                if (membership->seen)
                    continue;
                if (!membership->surface->client->failed)
                    wl_surface_send_leave (membership->surface->resource,
                                           binding->resource);
                wl_list_remove (&membership->link);
                free (membership);
            }
        }
    }
}

void
mb_outputs_surface_destroyed (MbSurface *surface)
{
    struct Output *output;
    struct OutputResource *binding;
    struct SurfaceOutput *membership, *next;

    if (!compositor.outputs)
        return;
    wl_list_for_each (output, &compositor.outputs->outputs, link)
    {
        wl_list_for_each (binding, &output->resources, link)
        {
            wl_list_for_each_safe (membership, next, &binding->surfaces, link)
            {
                if (membership->surface != surface)
                    continue;
                wl_list_remove (&membership->link);
                free (membership);
            }
        }
    }
}

void
mb_outputs_refresh (void)
{
    struct Output *output;
    int screen_index;

    if (!compositor.outputs)
        return;
    wl_list_for_each (output, &compositor.outputs->outputs, link)
    {
        output->seen = FALSE;
    }
    for (screen_index = 0; screen_index < screenInfo.numScreens;
         screen_index++)
    {
        ScreenPtr screen = screenInfo.screens[screen_index];
        rrScrPrivPtr randr =
            rrPrivKeyRec.initialized ? rrGetScrPriv (screen) : NULL;
        int i;

        if (randr && randr->numOutputs)
        {
            for (i = 0; i < randr->numOutputs; i++)
            {
                RROutputPtr native = randr->outputs[i];

                if (native->connection == RR_Connected && native->crtc &&
                    native->crtc->mode && !native->nonDesktop &&
                    !update_output (screen, native))
                    goto fail;
            }
        }
        else if (!update_output (screen, NULL))
            goto fail;
    }
    wl_list_for_each (output, &compositor.outputs->outputs, link)
    {
        if (!output->seen && !output->removed)
        {
            struct OutputResource *binding;

            output->removed = TRUE;
            wl_list_for_each (binding, &output->resources, link)
            {
                clear_memberships (binding, TRUE);
            }
            /* Keep the binding data alive for clients racing global removal.
             */
            wl_global_remove (output->global);
        }
    }
    refresh_surfaces ();
    return;

fail:
    mb_client_disconnect (compositor.bridge_client);
}

Bool
mb_outputs_initialize (void)
{
    compositor.outputs = calloc (1, sizeof (*compositor.outputs));
    if (!compositor.outputs)
        return FALSE;
    wl_list_init (&compositor.outputs->outputs);
    mb_outputs_refresh ();
    return !compositor.bridge_client->failed;
}

void
mb_outputs_shutdown (void)
{
    struct Output *output, *next;

    if (!compositor.outputs)
        return;
    wl_list_for_each_safe (output, next, &compositor.outputs->outputs, link)
    {
        struct OutputResource *binding, *next_binding;

        wl_global_destroy (output->global);
        wl_list_for_each_safe (binding, next_binding, &output->resources, link)
        {
            wl_resource_destroy (binding->resource);
        }
        wl_list_remove (&output->link);
        free (output);
    }
    free (compositor.outputs);
    compositor.outputs = NULL;
}
