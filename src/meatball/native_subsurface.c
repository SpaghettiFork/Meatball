/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "server.h"

#include <stdlib.h>
#include <wayland-server-protocol.h>
#include <window.h>

#define SUBCOMPOSITOR_VERSION 1
/* This protocol error is absent from older Wayland SDK headers. */
#define SUBCOMPOSITOR_BAD_PARENT 1

struct MbSubsurface
{
    struct wl_resource *resource;
    MbSurface *surface, *parent;
    struct wl_list pending_link;
    unsigned int refs;
    Bool synchronized, active;
    int32_t x, y, pending_x, pending_y;
    size_t stack_index;
};

static void
subsurface_unref (struct MbSubsurface *subsurface)
{
    if (!--subsurface->refs)
        free (subsurface);
}

static void
free_stack (MbPlacement *stack, size_t count)
{
    size_t i;

    if (!stack)
        return;
    for (i = 0; i < count; i++)
        if (stack[i].subsurface)
            subsurface_unref (stack[i].subsurface);
    free (stack);
}

static Bool
synchronized (MbSurface *surface)
{
    while (surface->subsurface)
    {
        struct MbSubsurface *subsurface = surface->subsurface;

        if (subsurface->synchronized)
            return TRUE;
        surface = subsurface->parent;
        if (!surface)
            break;
    }
    return FALSE;
}

MbSurface *
mb_subsurface_root (MbSurface *surface)
{
    while (surface && surface->role == MB_ROLE_SUBSURFACE)
    {
        struct MbSubsurface *subsurface = surface->subsurface;

        if (!subsurface || !subsurface->parent || !subsurface->active)
            return NULL;
        surface = subsurface->parent;
    }
    return surface;
}

MbSurface *
mb_scene_next (MbSceneIterator *iterator)
{
    while (iterator->surface && iterator->surface->pixels)
    {
        MbSurface *surface = iterator->surface;
        struct MbSubsurface *subsurface;
        MbPlacement *placement;

        if (!surface->stack_count && !iterator->index)
        {
            iterator->index++;
            return surface;
        }
        if (iterator->index >= surface->stack_count)
        {
            if (surface == iterator->root)
                break;
            subsurface = surface->subsurface;
            iterator->surface = subsurface->parent;
            iterator->index = subsurface->stack_index + 1;
            iterator->x -= subsurface->x;
            iterator->y -= subsurface->y;
            continue;
        }
        placement = &surface->stack[iterator->index++];
        subsurface = placement->subsurface;
        if (!subsurface)
            return surface;
        if (!subsurface->resource || !subsurface->surface ||
            subsurface->parent != surface || !subsurface->active ||
            !subsurface->surface->pixels)
            continue;
        iterator->x += placement->x;
        iterator->y += placement->y;
        if (iterator->x < INT32_MIN || iterator->x > INT32_MAX ||
            iterator->y < INT32_MIN || iterator->y > INT32_MAX)
        {
            iterator->failed = TRUE;
            break;
        }
        iterator->surface = subsurface->surface;
        iterator->index = 0;
    }
    iterator->surface = NULL;
    return NULL;
}

Bool
mb_subsurface_capture (MbSurfaceCommit *commit)
{
    MbSurface *surface = commit->surface;
    struct wl_list *link;
    size_t i = 0;

    commit->synchronized = synchronized (surface);
    commit->stack_count = wl_list_length (&surface->pending_stack);
    commit->stack = calloc (commit->stack_count, sizeof (*commit->stack));
    if (!commit->stack)
        return FALSE;
    for (link = surface->pending_stack.next; link != &surface->pending_stack;
         link = link->next, i++)
    {
        struct MbSubsurface *subsurface;
        MbSurfaceCommit *child;

        if (link == &surface->pending_self)
            continue;
        subsurface = wl_container_of (link, subsurface, pending_link);
        subsurface->refs++;
        commit->stack[i] = (MbPlacement){ subsurface, subsurface->pending_x,
                                          subsurface->pending_y };
        if (wl_list_empty (&subsurface->surface->commits))
            continue;
        child =
            wl_container_of (subsurface->surface->commits.prev, child, link);
        if (child->synchronized && !child->parent)
        {
            child->parent = commit;
            wl_list_insert (commit->dependencies.prev,
                            &child->dependency_link);
        }
    }
    return TRUE;
}

void
mb_subsurface_commit_free (MbSurfaceCommit *commit)
{
    MbSurfaceCommit *child, *next;

    wl_list_remove (&commit->dependency_link);
    wl_list_for_each_safe (child, next, &commit->dependencies, dependency_link)
    {
        wl_list_remove (&child->dependency_link);
        wl_list_init (&child->dependency_link);
        child->parent = NULL;
    }
    free_stack (commit->stack, commit->stack_count);
}

void
mb_subsurface_apply (MbSurfaceCommit *commit)
{
    MbSurface *surface = commit->surface;
    size_t i;

    for (i = 0; i < surface->stack_count; i++)
        if (surface->stack[i].subsurface)
            surface->stack[i].subsurface->active = FALSE;
    free_stack (surface->stack, surface->stack_count);
    surface->stack = commit->stack;
    surface->stack_count = commit->stack_count;
    commit->stack = NULL;
    commit->stack_count = 0;
    for (i = 0; i < surface->stack_count; i++)
    {
        struct MbSubsurface *subsurface = surface->stack[i].subsurface;

        if (!subsurface || !subsurface->resource || !subsurface->surface ||
            subsurface->parent != surface)
            continue;
        subsurface->active = TRUE;
        subsurface->x = surface->stack[i].x;
        subsurface->y = surface->stack[i].y;
        subsurface->stack_index = i;
    }
}

static void
apply_commits (MbSurfaceCommit *target)
{
    target->resume = NULL;
    while (target)
    {
        MbSurfaceCommit *commit =
            wl_container_of (target->surface->commits.next, commit, link);
        MbSurfaceCommit *resume;

        if (!wl_list_empty (&commit->dependencies))
        {
            MbSurfaceCommit *child = wl_container_of (
                commit->dependencies.next, child, dependency_link);

            child->resume = target;
            target = child;
            continue;
        }
        resume = commit == target ? target->resume : target;
        mb_surface_commit_apply (commit);
        mb_surface_commit_free (commit);
        target = resume;
    }
}

void
mb_subsurface_commit (MbSurfaceCommit *commit)
{
    MbSurface *surface = commit->surface;
    MbSurfaceCommit *previous = NULL, *child;

    if (commit->link.prev != &surface->commits)
        previous = wl_container_of (commit->link.prev, previous, link);
    if (previous && !previous->parent && previous->synchronized &&
        commit->synchronized)
    {
        wl_list_insert_list (&commit->frames, &previous->frames);
        wl_list_init (&previous->frames);
        wl_list_for_each (child, &previous->dependencies, dependency_link)
        {
            child->parent = commit;
        }
        wl_list_insert_list (&commit->dependencies, &previous->dependencies);
        wl_list_init (&previous->dependencies);
        mb_surface_commit_free (previous);
    }
    if (!commit->synchronized)
    {
        MbSurface *root;

        apply_commits (commit);
        root = mb_subsurface_root (surface);
        if (root)
            mb_surface_update (root);
    }
}

static void
flush_desynchronized (MbClient *client)
{
    MbSurface *surface;

    wl_list_for_each (surface, &compositor.surfaces, link)
    {
        MbSurfaceCommit *commit, *next;
        MbSurface *root;

        if (surface->client != client || synchronized (surface) ||
            wl_list_empty (&surface->commits))
            continue;
        wl_list_for_each_safe (commit, next, &surface->commits, link)
        {
            if (commit->parent)
            {
                wl_list_remove (&commit->dependency_link);
                wl_list_init (&commit->dependency_link);
                commit->parent = NULL;
            }
        }
        commit = wl_container_of (surface->commits.prev, commit, link);
        apply_commits (commit);
        root = mb_subsurface_root (surface);
        if (root)
            mb_surface_update (root);
    }
}

static void
detach (struct MbSubsurface *subsurface)
{
    wl_list_remove (&subsurface->pending_link);
    wl_list_init (&subsurface->pending_link);
    subsurface->active = FALSE;
    subsurface->parent = NULL;
}

static void
subsurface_destroyed (struct wl_resource *resource)
{
    struct MbSubsurface *subsurface = wl_resource_get_user_data (resource);
    MbSurface *surface = subsurface->surface;
    MbSurface *root = surface ? mb_subsurface_root (surface) : NULL;

    subsurface->resource = NULL;
    detach (subsurface);
    if (surface)
    {
        MbSurfaceCommit *commit, *next;

        surface->subsurface = NULL;
        subsurface->surface = NULL;
        wl_list_for_each_safe (commit, next, &surface->commits, link)
        {
            mb_surface_commit_free (commit);
        }
        flush_desynchronized (surface->client);
    }
    if (root && root != surface)
        mb_surface_update (root);
    subsurface_unref (subsurface);
}

void
mb_subsurface_surface_destroyed (MbSurface *surface)
{
    struct wl_list *link, *next;
    MbSurface *root = mb_subsurface_root (surface);

    if (surface->subsurface)
    {
        detach (surface->subsurface);
        surface->subsurface->surface = NULL;
        surface->subsurface = NULL;
    }
    for (link = surface->pending_stack.next; link != &surface->pending_stack;
         link = next)
    {
        struct MbSubsurface *subsurface;

        next = link->next;
        if (link == &surface->pending_self)
            continue;
        subsurface = wl_container_of (link, subsurface, pending_link);
        detach (subsurface);
    }
    free_stack (surface->stack, surface->stack_count);
    surface->stack = NULL;
    surface->stack_count = 0;
    if (root && root != surface)
        mb_surface_update (root);
}

static void
destroy (struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_destroy (resource);
}

static void
set_position (struct wl_client *client, struct wl_resource *resource,
              int32_t x, int32_t y)
{
    struct MbSubsurface *subsurface = wl_resource_get_user_data (resource);

    subsurface->pending_x = x;
    subsurface->pending_y = y;
}

static void
restack (struct wl_resource *resource, struct wl_resource *reference,
         Bool above)
{
    struct MbSubsurface *subsurface = wl_resource_get_user_data (resource);
    MbSurface *sibling = wl_resource_get_user_data (reference);
    struct wl_list *position;

    if (!subsurface->surface || !subsurface->parent)
        return;
    if (sibling == subsurface->parent)
        position = &sibling->pending_self;
    else if (sibling != subsurface->surface && sibling->subsurface &&
             sibling->subsurface->parent == subsurface->parent)
        position = &sibling->subsurface->pending_link;
    else
    {
        wl_resource_post_error (resource, WL_SUBSURFACE_ERROR_BAD_SURFACE,
                                "stack reference must be a sibling or parent");
        return;
    }
    wl_list_remove (&subsurface->pending_link);
    wl_list_insert (above ? position : position->prev,
                    &subsurface->pending_link);
}

static void
place_above (struct wl_client *client, struct wl_resource *resource,
             struct wl_resource *reference)
{
    restack (resource, reference, TRUE);
}

static void
place_below (struct wl_client *client, struct wl_resource *resource,
             struct wl_resource *reference)
{
    restack (resource, reference, FALSE);
}

static void
set_sync (struct wl_client *client, struct wl_resource *resource)
{
    struct MbSubsurface *subsurface = wl_resource_get_user_data (resource);

    subsurface->synchronized = TRUE;
}

static void
set_desync (struct wl_client *client, struct wl_resource *resource)
{
    struct MbSubsurface *subsurface = wl_resource_get_user_data (resource);

    subsurface->synchronized = FALSE;
    if (subsurface->surface)
        flush_desynchronized (subsurface->surface->client);
}

static const struct wl_subsurface_interface subsurface_implementation = {
    .destroy = destroy,
    .set_position = set_position,
    .place_above = place_above,
    .place_below = place_below,
    .set_sync = set_sync,
    .set_desync = set_desync,
};

static void
get_subsurface (struct wl_client *client, struct wl_resource *resource,
                uint32_t id, struct wl_resource *surface_resource,
                struct wl_resource *parent_resource)
{
    MbSurface *surface = wl_resource_get_user_data (surface_resource);
    MbSurface *parent = wl_resource_get_user_data (parent_resource);
    MbSurface *ancestor;
    struct MbSubsurface *subsurface;
    WindowPtr window;

    if (surface->subsurface || surface->xdg ||
        (surface->role != MB_ROLE_NONE && surface->role != MB_ROLE_SUBSURFACE))
    {
        wl_resource_post_error (
            resource, WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE,
            "surface already has a role or subsurface object");
        return;
    }
    for (ancestor = parent; ancestor;
         ancestor = ancestor->subsurface ? ancestor->subsurface->parent : NULL)
    {
        if (ancestor == surface)
        {
            wl_resource_post_error (resource, SUBCOMPOSITOR_BAD_PARENT,
                                    "subsurface parent would create a cycle");
            return;
        }
    }
    subsurface = calloc (1, sizeof (*subsurface));
    if (!subsurface)
    {
        wl_resource_post_no_memory (resource);
        return;
    }
    subsurface->resource = wl_resource_create (
        client, &wl_subsurface_interface, SUBCOMPOSITOR_VERSION, id);
    if (!subsurface->resource)
    {
        free (subsurface);
        wl_resource_post_no_memory (resource);
        return;
    }
    subsurface->refs = 1;
    subsurface->surface = surface;
    subsurface->parent = parent;
    subsurface->synchronized = TRUE;
    surface->role = MB_ROLE_SUBSURFACE;
    surface->subsurface = subsurface;
    wl_list_insert (parent->pending_stack.prev, &subsurface->pending_link);
    wl_resource_set_implementation (subsurface->resource,
                                    &subsurface_implementation, subsurface,
                                    subsurface_destroyed);
    window = mb_window_get (&surface->window, DixSetAttrAccess);
    if (window)
        UnmapWindow (window, FALSE);
}

static const struct wl_subcompositor_interface implementation = {
    .destroy = destroy,
    .get_subsurface = get_subsurface,
};

static void
bind_subcompositor (struct wl_client *client, void *data, uint32_t version,
                    uint32_t id)
{
    struct wl_resource *resource =
        wl_resource_create (client, &wl_subcompositor_interface, version, id);

    if (!resource)
    {
        wl_client_post_no_memory (client);
        return;
    }
    wl_resource_set_implementation (resource, &implementation, NULL, NULL);
}

Bool
mb_subsurfaces_initialize (void)
{
    compositor.subsurface_global =
        wl_global_create (compositor.wl_display, &wl_subcompositor_interface,
                          SUBCOMPOSITOR_VERSION, NULL, bind_subcompositor);
    return compositor.subsurface_global != NULL;
}

void
mb_subsurfaces_shutdown (void)
{
    if (compositor.subsurface_global)
    {
        wl_global_destroy (compositor.subsurface_global);
        compositor.subsurface_global = NULL;
    }
}
