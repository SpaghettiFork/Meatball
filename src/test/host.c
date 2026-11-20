/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <xorg-server.h>

#include <dlfcn.h>
#include <extension.h>
#include <stdlib.h>
#include <string.h>
#include <xf86Module.h>
#include <xf86Optionstr.h>

#ifdef DRI3
#include <dixstruct.h>
#include <dri3.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <pixmapstr.h>
#include <shmint.h>
#endif

#define MODULE_PATH_ENV "MEATBALL_TEST_MODULE"
#define MODULE_DATA_SYMBOL "sauceModuleData"
#define SOCKET_PATH_ENV "MEATBALL_TEST_SOCKET"
#define SOCKET_MODE_ENV "MEATBALL_TEST_SOCKET_MODE"

extern void __real_InitExtensions (int argc, char **argv);
extern void __wrap_InitExtensions (int argc, char **argv);

#ifdef DRI3
#define TEST_DRI3_ENV "MEATBALL_TEST_DRI3"
#define TEST_DRI3_DRIVER_VERSION 2
#define TEST_DRI3_NO_DEVICE "no-device"
#define TEST_DRI3_ASYNC "async"
#define TEST_DEVICE "/dev/null"
#define TEST_ARGB_DEPTH 32

static DevPrivateKeyRec test_dri3_pixmap_key;
static GetImageProcPtr test_dri3_get_image;

static Bool
test_dri3_open_ready (ClientPtr client, void *data)
{
    if (!client->clientGone)
    {
        int fd = open (TEST_DEVICE, O_RDWR | O_CLOEXEC);

        if (fd < 0 || dri3_send_open_reply (client, fd) != Success)
            FatalError ("meatball-test: failed to send DRI3 open reply\n");
        AttendClient (client);
    }
    return TRUE;
}

static int
test_dri3_open (ClientPtr client, ScreenPtr screen, RRProviderPtr provider,
                int *fd)
{
    const char *mode = getenv (TEST_DRI3_ENV);

    if (strcmp (mode, TEST_DRI3_NO_DEVICE) == 0)
        return BadMatch;
    if (strcmp (mode, TEST_DRI3_ASYNC) == 0)
    {
        if (!QueueWorkProc (test_dri3_open_ready, client, NULL))
            return BadAlloc;
        IgnoreClient (client);
        return Success;
    }
    /* A character device stands in for DRM so feedback can be checked. */
    *fd = open (TEST_DEVICE, O_RDWR | O_CLOEXEC);
    return *fd < 0 ? BadAlloc : Success;
}

static int
test_dri3_formats (ScreenPtr screen, CARD32 *count, CARD32 **formats)
{
    static const CARD32 supported[] = { DRM_FORMAT_XRGB8888,
                                        DRM_FORMAT_ARGB8888,
                                        DRM_FORMAT_RGB565 };

    *formats = malloc (sizeof (supported));
    if (!*formats)
        return FALSE;
    memcpy (*formats, supported, sizeof (supported));
    *count = ARRAY_SIZE (supported);
    return TRUE;
}

static int
test_dri3_modifiers (ScreenPtr screen, uint32_t format, uint32_t *count,
                     uint64_t **modifiers)
{
    static const uint64_t supported[] = {
        DRM_FORMAT_MOD_LINEAR,
        DRM_FORMAT_MOD_INVALID,
    };

    *modifiers = malloc (sizeof (supported));
    if (!*modifiers)
        return FALSE;
    memcpy (*modifiers, supported, sizeof (supported));
    *count = format == DRM_FORMAT_XRGB8888 ? ARRAY_SIZE (supported) : 1;
    return TRUE;
}

static void
test_dri3_check_image (DrawablePtr drawable, int x, int y, int width,
                       int height, unsigned int format, unsigned long mask,
                       char *pixels)
{
    if (drawable->type == DRAWABLE_PIXMAP &&
        dixLookupPrivate (&((PixmapPtr)drawable)->devPrivates,
                          &test_dri3_pixmap_key))
        FatalError ("meatball-test: DMA-BUF snapshot used GetImage\n");
    test_dri3_get_image (drawable, x, y, width, height, format, mask, pixels);
}

static PixmapPtr
test_dri3_import (ScreenPtr screen, CARD8 count, const int *fds, CARD16 width,
                  CARD16 height, const CARD32 *strides, const CARD32 *offsets,
                  CARD8 depth, CARD8 bpp, CARD64 modifier)
{
    PixmapPtr pixmap;

    if (count != 1)
        return NULL;
    /* A test-only driver backed by the server's guarded SHM pixmaps. */
    pixmap = shm_create_pixmap_from_fd (screen, fds[0], width, height, depth,
                                        bpp, strides[0], offsets[0]);
    /* The ARGB fixture uses the Y-inversion readback path. */
    if (pixmap && depth != TEST_ARGB_DEPTH)
        dixSetPrivate (&pixmap->devPrivates, &test_dri3_pixmap_key, pixmap);
    return pixmap;
}

static const dri3_screen_info_rec test_dri3 = {
    .version = TEST_DRI3_DRIVER_VERSION,
    .open_client = test_dri3_open,
    .pixmap_from_fds = test_dri3_import,
    .get_formats = test_dri3_formats,
    .get_modifiers = test_dri3_modifiers,
};
#endif

void
__wrap_InitExtensions (int argc, char **argv)
{
    static void *module;

    if (!module)
    {
        const char *path = getenv (MODULE_PATH_ENV);
        XF86ModuleData *data;
        XF86OptionRec mode = { .opt_name = "SocketMode",
                               .opt_val = getenv (SOCKET_MODE_ENV) };
        XF86OptionRec path_option = {
            .opt_name = "SocketPath",
            .opt_val = getenv (SOCKET_PATH_ENV),
            .list.next = mode.opt_val ? &mode : NULL,
        };
        int major = 0, minor = 0;

        if (!path)
            FatalError ("meatball-test: %s is required\n", MODULE_PATH_ENV);
        module = dlopen (path, RTLD_NOW | RTLD_GLOBAL);
        if (!module)
            FatalError ("meatball-test: %s\n", dlerror ());
        data = dlsym (module, MODULE_DATA_SYMBOL);
        if (!data || !data->vers || !data->setup ||
            strcmp (data->vers->abiclass, ABI_CLASS_EXTENSION) ||
            data->vers->abiversion != ABI_EXTENSION_VERSION ||
            data->vers->xf86version != XORG_VERSION_CURRENT)
            FatalError ("meatball-test: incompatible module metadata\n");
        if (!data->setup (module, path_option.opt_val ? &path_option : NULL,
                          &major, &minor))
            FatalError ("meatball-test: module setup failed (%d, %d)\n", major,
                        minor);
    }

#ifdef DRI3
    if (getenv (TEST_DRI3_ENV))
    {
        ScreenPtr screen = screenInfo.screens[0];

        if (!dri3_screen_init (screen, &test_dri3) ||
            !dixRegisterPrivateKey (&test_dri3_pixmap_key, PRIVATE_PIXMAP, 0))
            FatalError (
                "meatball-test: DRI3 test driver initialization failed\n");
        test_dri3_get_image = screen->GetImage;
        screen->GetImage = test_dri3_check_image;
    }
#endif
    __real_InitExtensions (argc, argv);
}
