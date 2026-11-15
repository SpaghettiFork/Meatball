/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "server.h"

#include <errno.h>
#include <extnsionst.h>
#include <fcntl.h>
#include <gcstruct.h>
#include <picturestr.h>
#include <pixmapstr.h>
#include <servermd.h>
#include <shmint.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wayland-server-protocol.h>

#define MB_SHM_VERSION 1
#define SHM_EXTENSION_NAME "MIT-SHM"
#define RENDER_EXTENSION_NAME "RENDER"
#define MB_ALL_PLANES (~0UL)

struct MbShmPool
{
    int fd;
    int32_t size;
    unsigned int refs;
};

static void
pool_unref (struct MbShmPool *pool)
{
    if (!--pool->refs)
    {
        close (pool->fd);
        free (pool);
    }
}

void
mb_shm_buffer_unref (MbShmBuffer *buffer)
{
    if (!--buffer->refs)
    {
        pool_unref (buffer->pool);
        free (buffer);
    }
}

static void
buffer_destroyed (struct wl_resource *resource)
{
    MbShmBuffer *buffer = wl_resource_get_user_data (resource);

    buffer->resource = NULL;
    mb_shm_buffer_unref (buffer);
}

static void
buffer_destroy (struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_destroy (resource);
}

static const struct wl_buffer_interface buffer_implementation = {
    .destroy = buffer_destroy,
};

MbShmBuffer *
mb_shm_buffer_acquire (struct wl_resource *resource)
{
    MbShmBuffer *buffer;

    if (!resource || !wl_resource_instance_of (resource, &wl_buffer_interface,
                                               &buffer_implementation))
        return NULL;
    buffer = wl_resource_get_user_data (resource);
    buffer->refs++;
    return buffer;
}

static void
pool_destroyed (struct wl_resource *resource)
{
    pool_unref (wl_resource_get_user_data (resource));
}

static void
pool_destroy (struct wl_client *client, struct wl_resource *resource)
{
    wl_resource_destroy (resource);
}

static void
pool_resize (struct wl_client *client, struct wl_resource *resource,
             int32_t size)
{
    struct MbShmPool *pool = wl_resource_get_user_data (resource);
    struct stat info;

    if (size <= pool->size || fstat (pool->fd, &info) < 0 ||
        info.st_size < size)
    {
        wl_resource_post_error (resource, WL_SHM_ERROR_INVALID_FD,
                                "invalid pool resize");
        return;
    }
    pool->size = size;
}

static void
pool_create_buffer (struct wl_client *client, struct wl_resource *resource,
                    uint32_t id, int32_t offset, int32_t width, int32_t height,
                    int32_t stride, uint32_t format)
{
    struct MbShmPool *pool = wl_resource_get_user_data (resource);
    MbShmBuffer *buffer;

    if (format != WL_SHM_FORMAT_ARGB8888 && format != WL_SHM_FORMAT_XRGB8888)
    {
        wl_resource_post_error (resource, WL_SHM_ERROR_INVALID_FORMAT,
                                "unsupported SHM format");
        return;
    }
    if (width <= 0 || height <= 0 || width > MB_MAX_DIMENSION ||
        height > MB_MAX_DIMENSION ||
        stride < (int64_t)width * MB_SHM_BYTES_PER_PIXEL ||
        stride % MB_SHM_BYTES_PER_PIXEL || offset < 0 ||
        offset % MB_SHM_BYTES_PER_PIXEL || offset > pool->size ||
        (int64_t)stride * height > pool->size - offset)
    {
        wl_resource_post_error (resource, WL_SHM_ERROR_INVALID_STRIDE,
                                "invalid SHM buffer extent");
        return;
    }
    buffer = calloc (1, sizeof (*buffer));
    if (!buffer)
    {
        wl_resource_post_no_memory (resource);
        return;
    }
    buffer->resource =
        wl_resource_create (client, &wl_buffer_interface, 1, id);
    if (!buffer->resource)
    {
        free (buffer);
        wl_resource_post_no_memory (resource);
        return;
    }
    buffer->pool = pool;
    pool->refs++;
    buffer->refs = 1;
    buffer->width = width;
    buffer->height = height;
    buffer->stride = stride;
    buffer->offset = offset;
    buffer->format = format;
    wl_resource_set_implementation (buffer->resource, &buffer_implementation,
                                    buffer, buffer_destroyed);
}

static const struct wl_shm_pool_interface pool_implementation = {
    .create_buffer = pool_create_buffer,
    .destroy = pool_destroy,
    .resize = pool_resize,
};

static void
create_pool (struct wl_client *client, struct wl_resource *resource,
             uint32_t id, int fd, int32_t size)
{
    struct stat info;
    struct MbShmPool *pool;
    struct wl_resource *result;
    int flags = fcntl (fd, F_GETFD);
    int access = fcntl (fd, F_GETFL);

    if (size <= 0 || flags < 0 || access < 0 ||
        (access & O_ACCMODE) != O_RDWR ||
        fcntl (fd, F_SETFD, flags | FD_CLOEXEC) < 0 || fstat (fd, &info) < 0 ||
        info.st_size < size)
    {
        close (fd);
        wl_resource_post_error (resource, WL_SHM_ERROR_INVALID_FD,
                                "invalid SHM pool fd or size");
        return;
    }
    pool = calloc (1, sizeof (*pool));
    if (!pool)
    {
        close (fd);
        wl_resource_post_no_memory (resource);
        return;
    }
    result = wl_resource_create (client, &wl_shm_pool_interface,
                                 MB_SHM_VERSION, id);
    if (!result)
    {
        close (fd);
        free (pool);
        wl_resource_post_no_memory (resource);
        return;
    }
    pool->refs = 1;
    pool->fd = fd;
    pool->size = size;
    wl_resource_set_implementation (result, &pool_implementation, pool,
                                    pool_destroyed);
}

static const struct wl_shm_interface implementation = {
    .create_pool = create_pool,
};

static void
bind_shm (struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    struct wl_resource *resource =
        wl_resource_create (client, &wl_shm_interface, MB_SHM_VERSION, id);

    if (!resource)
    {
        wl_client_post_no_memory (client);
        return;
    }
    wl_resource_set_implementation (resource, &implementation, NULL, NULL);
    wl_shm_send_format (resource, WL_SHM_FORMAT_ARGB8888);
    wl_shm_send_format (resource, WL_SHM_FORMAT_XRGB8888);
}

Bool
mb_shm_initialize (void)
{
    int i;

    if (!CheckExtension (SHM_EXTENSION_NAME) ||
        !CheckExtension (RENDER_EXTENSION_NAME))
    {
        mb_log (X_ERROR,
                "meatball: SHM rendering requires MIT-SHM and RENDER\n");
        errno = ENOTSUP;
        return FALSE;
    }
    for (i = 0; i < screenInfo.numScreens; i++)
    {
        ScreenPtr screen = screenInfo.screens[i];

        if (!PictureMatchFormat (screen, MB_SHM_ARGB_DEPTH, PICT_a8r8g8b8) ||
            !PictureMatchFormat (screen, MB_SHM_XRGB_DEPTH, PICT_x8r8g8b8))
        {
            mb_log (X_ERROR,
                    "meatball: Screen %d lacks required Render formats\n", i);
            errno = ENOTSUP;
            return FALSE;
        }
    }
    compositor.shm_global =
        wl_global_create (compositor.wl_display, &wl_shm_interface,
                          MB_SHM_VERSION, NULL, bind_shm);
    return compositor.shm_global != NULL;
}

void
mb_shm_shutdown (void)
{
    if (compositor.shm_global)
    {
        wl_global_destroy (compositor.shm_global);
        compositor.shm_global = NULL;
    }
}

PixmapPtr
mb_shm_snapshot (MbShmBuffer *buffer, ScreenPtr screen)
{
    unsigned int depth = buffer->format == WL_SHM_FORMAT_ARGB8888
                             ? MB_SHM_ARGB_DEPTH
                             : MB_SHM_XRGB_DEPTH;
    size_t stride = PixmapBytePad (buffer->width, depth);
    size_t size = stride * buffer->height;
    PixmapPtr imported, copy = NULL;
    GCPtr gc = NULL;
    char *pixels;

    if (size > MB_SHM_MAX_SNAPSHOT_BYTES)
        return NULL;
    pixels = malloc (size);
    if (!pixels)
        return NULL;
    imported = shm_create_pixmap_from_fd (
        screen, buffer->pool->fd, buffer->width, buffer->height, depth,
        MB_SHM_BITS_PER_PIXEL, buffer->stride, buffer->offset);
    if (!imported)
        goto done;
    /* The imported pixmap uses Spaghetti's SIGBUS protection. Copy through
     * the screen APIs so client memory is consumed before buffer release.
     */
    screen->GetImage (&imported->drawable, 0, 0, buffer->width, buffer->height,
                      ZPixmap, MB_ALL_PLANES, pixels);
    screen->DestroyPixmap (imported);
    copy =
        screen->CreatePixmap (screen, buffer->width, buffer->height, depth, 0);
    if (!copy)
        goto done;
    gc = GetScratchGC (depth, screen);
    if (!gc)
    {
        screen->DestroyPixmap (copy);
        copy = NULL;
        goto done;
    }
    ValidateGC (&copy->drawable, gc);
    gc->ops->PutImage (&copy->drawable, gc, depth, 0, 0, buffer->width,
                       buffer->height, 0, ZPixmap, pixels);
    FreeScratchGC (gc);

done:
    free (pixels);
    return copy;
}
