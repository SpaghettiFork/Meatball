/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "server.h"

#include <assert.h>
#include <errno.h>
#include <extension.h>
#include <extnsionst.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <xf86Module.h>

#define PROBE_EXTENSION "MEATBALL_LOOP_PROBE"
#define SOCKET_PATH_ENV "MEATBALL_TEST_SOCKET"
#define RESULT_PATH_ENV "MEATBALL_TEST_RESULT"
#define TIMER_DELAY_MS 1
#define TIMER_TICKS 3
#define PIPE_CLOSED (-1)

static struct wl_event_source *timer_source;
static struct wl_event_source *read_source;
static int pipe_fds[2] = { PIPE_CLOSED, PIPE_CLOSED };
static unsigned int ticks;
static Bool idle_ran;

static int
probe_read (int fd, uint32_t mask, void *data)
{
    char buffer[TIMER_TICKS];
    ssize_t count;

    do
        count = read (fd, buffer, sizeof (buffer));
    while (count > 0 || (count < 0 && errno == EINTR));
    if (count == 0)
    {
        int result;
        const char complete[] = "complete";

        assert (idle_ran && ticks == TIMER_TICKS);
        wl_event_source_remove (read_source);
        read_source = NULL;
        close (pipe_fds[0]);
        pipe_fds[0] = PIPE_CLOSED;
        result = open (getenv (RESULT_PATH_ENV), O_CREAT | O_EXCL | O_WRONLY,
                       MEATBALL_DEFAULT_SOCKET_MODE);
        assert (result >= 0);
        assert (write (result, complete, sizeof (complete)) ==
                sizeof (complete));
        close (result);
    }
    else
        assert (errno == EAGAIN);
    return 0;
}

static int
probe_timer (void *data)
{
    const char tick = 0;

    assert (write (pipe_fds[1], &tick, sizeof (tick)) == sizeof (tick));
    if (++ticks == TIMER_TICKS)
    {
        wl_event_source_remove (timer_source);
        timer_source = NULL;
        close (pipe_fds[1]);
        pipe_fds[1] = PIPE_CLOSED;
    }
    else
        assert (wl_event_source_timer_update (timer_source, TIMER_DELAY_MS) ==
                0);
    return 0;
}

static void
probe_idle (void *data)
{
    idle_ran = TRUE;
}

static void
probe_wakeup (void *data, int result)
{
}

static void
probe_start (void *data, void *timeout)
{
    struct meatball_config config = {
        .socket_path = getenv (SOCKET_PATH_ENV),
        .socket_mode = MEATBALL_DEFAULT_SOCKET_MODE,
    };

    RemoveBlockAndWakeupHandlers (probe_start, probe_wakeup, NULL);
    assert (meatball_initialize (&config));
    assert (pipe2 (pipe_fds, O_NONBLOCK | O_CLOEXEC) == 0);
    read_source = wl_event_loop_add_fd (compositor.wl_event_loop, pipe_fds[0],
                                        WL_EVENT_READABLE, probe_read, NULL);
    timer_source =
        wl_event_loop_add_timer (compositor.wl_event_loop, probe_timer, NULL);
    assert (read_source && timer_source);
    assert (
        wl_event_loop_add_idle (compositor.wl_event_loop, probe_idle, NULL));
    assert (wl_event_source_timer_update (timer_source, TIMER_DELAY_MS) == 0);
}

static void
probe_reset (ExtensionEntry *extension)
{
    unsigned int i;

    if (read_source)
        wl_event_source_remove (read_source);
    if (timer_source)
        wl_event_source_remove (timer_source);
    read_source = timer_source = NULL;
    for (i = 0; i < ARRAY_SIZE (pipe_fds); i++)
    {
        if (pipe_fds[i] != PIPE_CLOSED)
            close (pipe_fds[i]);
        pipe_fds[i] = PIPE_CLOSED;
    }
    meatball_shutdown ();
}

static int
probe_request (ClientPtr client)
{
    return BadRequest;
}

static void
probe_initialize (void)
{
    assert (AddExtension (PROBE_EXTENSION, 0, 0, probe_request, probe_request,
                          probe_reset, StandardMinorOpcode));
    assert (RegisterBlockAndWakeupHandlers (probe_start, probe_wakeup, NULL));
}

static void *
probe_setup (void *module, void *opts, int *errmaj, int *errmin)
{
    static const ExtensionModule extension = {
        .initFunc = probe_initialize,
        .name = PROBE_EXTENSION,
    };

    LoadExtensionList (&extension, 1, FALSE);
    return module;
}

static XF86ModuleVersionInfo version = {
    .modname = "meatball-loop-probe",
    .vendor = "Spaghetti Fork",
    ._modinfo1_ = MODINFOSTRING1,
    ._modinfo2_ = MODINFOSTRING2,
    .xf86version = XORG_VERSION_CURRENT,
    .abiclass = ABI_CLASS_EXTENSION,
    .abiversion = ABI_EXTENSION_VERSION,
};

_X_EXPORT XF86ModuleData sauceModuleData = {
    .vers = &version,
    .setup = probe_setup,
};
