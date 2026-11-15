/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "probe.h"

#include <assert.h>
#include <fcntl.h>
#include <inputstr.h>
#include <unistd.h>
#include <xkbsrv.h>

#define TEST_INTERFACE "meatball_keymap_test"
#define TEST_VERSION 1
#define RESULT_EVENT 0
#define LAYOUT_US 0
#define LAYOUT_DE 1

static struct wl_global *test_global;

static void
snapshot (struct wl_client *client, struct wl_resource *resource,
          uint32_t layout)
{
    DeviceIntPtr device;
    char rules_name[] = "evdev", model[] = "pc105";
    char layout_us[] = "us", layout_de[] = "de";
    XkbRMLVOSet rules = { .rules = rules_name,
                          .model = model,
                          .layout =
                              layout == LAYOUT_DE ? layout_de : layout_us };
    XkbDescPtr keymap;
    unsigned short defined;
    uint32_t size;
    int fd;

    assert (layout == LAYOUT_US || layout == LAYOUT_DE);
    for (device = inputInfo.devices; device; device = device->next)
        if (device->type == MASTER_KEYBOARD && device->key)
            break;
    assert (device);
    keymap = XkbCompileKeymap (device, &rules);
    assert (keymap);
    assert (XkbDeviceApplyKeymap (device, keymap));
    XkbFreeKeyboard (keymap, XkbAllComponentsMask, TRUE);
    defined = device->key->xkbInfo->desc->defined;
    fd = mb_keymap_create (device, &size);
    assert (fd >= 0 && size > 0);
    assert (device->key->xkbInfo->desc->defined == defined);
    assert (fcntl (fd, F_GETFD) & FD_CLOEXEC);
    assert ((fcntl (fd, F_GET_SEALS) & F_SEAL_WRITE) == F_SEAL_WRITE);
    wl_resource_post_event (resource, RESULT_EVENT, fd, size, device->id);
    close (fd);
}

static const struct wl_message requests[] = { { "snapshot", "u", NULL } };
static const struct wl_message events[] = { { "keymap", "huu", NULL } };
static const struct wl_interface interface = {
    TEST_INTERFACE, TEST_VERSION,        ARRAY_SIZE (requests),
    requests,       ARRAY_SIZE (events), events,
};
static const struct
{
    void (*snapshot) (struct wl_client *, struct wl_resource *, uint32_t);
} implementation = { snapshot };

static void
bind_test (struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    struct wl_resource *resource =
        wl_resource_create (client, &interface, version, id);

    assert (resource);
    wl_resource_set_implementation (resource, &implementation, NULL, NULL);
}

void
mb_probe_start (void)
{
    uint32_t size = 1;

    assert (mb_keymap_create (NULL, &size) == -1 && size == 0);
    test_global = wl_global_create (compositor.wl_display, &interface,
                                    TEST_VERSION, NULL, bind_test);
    assert (test_global);
}

void
mb_probe_stop (void)
{
    if (test_global)
        wl_global_destroy (test_global);
    test_global = NULL;
}
