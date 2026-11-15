/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "probe.h"

#include <assert.h>
#include <randrstr.h>
#include <string.h>

#define TEST_INTERFACE "meatball_output_test"
#define TEST_VERSION 1
#define APPLIED_EVENT 0
#define MODE_NAME "meatball-output-test"
#define SECOND_OUTPUT_NAME "meatball-second"
#define OUTPUT_WIDTH 64
#define OUTPUT_HEIGHT 128
#define MODE_HTOTAL 80
#define MODE_VTOTAL 150
#define MODE_DOTCLOCK 720000
#define PHYSICAL_WIDTH 320
#define PHYSICAL_HEIGHT 180

enum Operation
{
    SPLIT,
    ROTATION,
    CONNECTION,
    DESTROY_SECOND,
    MODE_FLAGS,
    POSITION,
    NON_DESKTOP,
    PHYSICAL,
    OFF,
};

static struct wl_global *test_global;
static RROutputPtr first, second;
static RRCrtcPtr first_crtc, second_crtc;

static RRModePtr
mode_for_flags (CARD32 flags)
{
    xRRModeInfo info = { .width = OUTPUT_WIDTH,
                         .height = OUTPUT_HEIGHT,
                         .hTotal = MODE_HTOTAL,
                         .vTotal = MODE_VTOTAL,
                         .dotClock = MODE_DOTCLOCK,
                         .modeFlags = flags,
                         .nameLength = sizeof (MODE_NAME) - 1 };
    RRModePtr mode = RRModeGet (&info, MODE_NAME);

    assert (mode);
    return mode;
}

static void
configure (struct wl_client *client, struct wl_resource *resource,
           uint32_t operation, uint32_t argument)
{
    RRModePtr mode;
    ScreenPtr screen = screenInfo.screens[0];

    switch (operation)
    {
    case SPLIT:
        mode = mode_for_flags (0);
        assert (RROutputSetModes (first, &mode, 1, 1));
        assert (RRCrtcNotify (first_crtc, mode, 0, 0, RR_Rotate_0, NULL, 1,
                              &first));
        if (!second)
        {
            second = RROutputCreate (screen, SECOND_OUTPUT_NAME,
                                     sizeof (SECOND_OUTPUT_NAME) - 1, NULL);
            second_crtc = RRCrtcCreate (screen, NULL);
            assert (second && second_crtc);
            assert (RROutputSetCrtcs (second, &second_crtc, 1));
            assert (RROutputSetConnection (second, RR_Connected));
        }
        mode = mode_for_flags (0);
        assert (RROutputSetModes (second, &mode, 1, 0));
        assert (RRCrtcNotify (second_crtc, mode, OUTPUT_WIDTH, 0, RR_Rotate_0,
                              NULL, 1, &second));
        break;
    case ROTATION:
        assert (RRCrtcNotify (first_crtc, first_crtc->mode, 0, 0, argument,
                              NULL, 1, &first));
        break;
    case CONNECTION:
        assert (RROutputSetConnection (first, argument ? RR_Connected
                                                       : RR_Disconnected));
        break;
    case DESTROY_SECOND:
        assert (second);
        assert (RRCrtcNotify (second_crtc, NULL, 0, 0, RR_Rotate_0, NULL, 0,
                              NULL));
        RROutputDestroy (second);
        RRCrtcDestroy (second_crtc);
        second = NULL;
        second_crtc = NULL;
        break;
    case MODE_FLAGS:
        mode = mode_for_flags (argument);
        assert (RROutputSetModes (first, &mode, 1, 1));
        assert (RRCrtcNotify (first_crtc, mode, 0, 0, RR_Rotate_0, NULL, 1,
                              &first));
        break;
    case POSITION:
        assert (RRCrtcNotify (first_crtc, first_crtc->mode, (int32_t)argument,
                              0, first_crtc->rotation, NULL, 1, &first));
        break;
    case NON_DESKTOP:
        assert (RROutputSetNonDesktop (first, !!argument));
        break;
    case PHYSICAL:
        assert (
            RROutputSetPhysicalSize (first, PHYSICAL_WIDTH, PHYSICAL_HEIGHT));
        assert (RROutputSetSubpixelOrder (first, SubPixelHorizontalRGB));
        break;
    case OFF:
        assert (
            RRCrtcNotify (first_crtc, NULL, 0, 0, RR_Rotate_0, NULL, 0, NULL));
        break;
    default:
        assert (FALSE);
    }
    RRTellChanged (screen);
    wl_resource_post_event (resource, APPLIED_EVENT, operation);
}

static const struct wl_message requests[] = { { "configure", "uu", NULL } };
static const struct wl_message events[] = { { "applied", "u", NULL } };
static const struct wl_interface interface = {
    TEST_INTERFACE, TEST_VERSION,        ARRAY_SIZE (requests),
    requests,       ARRAY_SIZE (events), events,
};
static const struct
{
    void (*configure) (struct wl_client *, struct wl_resource *, uint32_t,
                       uint32_t);
} implementation = { configure };

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
    rrScrPrivPtr randr = rrGetScrPriv (screenInfo.screens[0]);

    assert (randr && randr->numOutputs == 1);
    first = randr->outputs[0];
    first_crtc = first->crtc;
    second = NULL;
    second_crtc = NULL;
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
    first = second = NULL;
    first_crtc = second_crtc = NULL;
}
