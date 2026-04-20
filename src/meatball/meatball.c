/**
 * Spaghetti Display Server
 * Copyright (C) 2025  SpaghettiFork
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <locale.h>
#include <unistd.h>
#include <stdio.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

/* Meatball interface is imported via compositor.h */
#include "compositor.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* GLOBAL */ Compositor compositor;

typedef Bool (*WaylandFeatureInit)(void);

typedef struct {
    WaylandFeatureInit func;
    const char *name;
} WaylandFeature;

static const WaylandFeature functions[] =
{
#define ADD_FUNC(f) { f, #f }
	ADD_FUNC(XLInitRROutputs),
	ADD_FUNC(XLInitCompositor),
	ADD_FUNC(XLInitSurfaces),
	ADD_FUNC(XLInitShm),
	ADD_FUNC(XLInitXdgWM),
	ADD_FUNC(XLInitXdgSurfaces),
	ADD_FUNC(XLInitXdgToplevels),
	ADD_FUNC(XLInitSystemBell),
	ADD_FUNC(XLInitFrameClock),
	ADD_FUNC(XLInitSubsurfaces),
	ADD_FUNC(XLInitSeats),
	ADD_FUNC(XLInitDataDevice),
	ADD_FUNC(XLInitPopups),
	ADD_FUNC(XLInitDmabuf),
	ADD_FUNC(XLInitXData),
	ADD_FUNC(XLInitXSettings),
	ADD_FUNC(XLInitIconSurfaces),
	ADD_FUNC(XLInitPrimarySelection),
	ADD_FUNC(XLInitExplicitSynchronization),
	ADD_FUNC(XLInitWpViewporter),
	ADD_FUNC(XLInitDecoration),
	ADD_FUNC(XLInitTextInput),
	ADD_FUNC(XLInitSinglePixelBuffer),
	ADD_FUNC(XLInitDrmLease),
	ADD_FUNC(XLInitPointerConstraints),
	ADD_FUNC(XLInitRelativePointer),
	ADD_FUNC(XLInitKeyboardShortcutsInhibit),
	ADD_FUNC(XLInitIdleInhibit),
	ADD_FUNC(XLInitPointerGestures),
	ADD_FUNC(XLInitXdgActivation),
	ADD_FUNC(XLInitTearingControl),
	ADD_FUNC(XLInitTest)
#undef ADD_FUNC
};

static void
DetermineServerTime(void)
{
	struct timespec clock_spec;
	uint64_t clock_ms, truncated;

	/* Try to determine if the X server time is the same as the
	   monotonic time.  If it is not, certain features such as "active"
	   frame synchronization will not be available.  */

	clock_gettime(CLOCK_MONOTONIC, &clock_spec);
	const Time server_time = XLGetServerTimeRoundtrip();

	/* Convert the monotonic time to milliseconds.  */

	if (IntMultiplyWrapv(clock_spec.tv_sec, 1000, &clock_ms))
		goto overflow;

	if (IntAddWrapv(clock_ms, clock_spec.tv_nsec / 1000000,
					&clock_ms))
		goto overflow;

	/* Truncate the time to 32 bits.  */
	truncated = clock_ms;

	/* Compare the clock time with the server time.  */
	if (llabs((long long)server_time - (long long)truncated) <= 5)
	{
		/* Since the difference between the server time and the monotonic
		   time is less than 5 ms, the server time is most likely the
		   monotonic time.  */
		compositor.server_time_monotonic = True;
	}
	else
	{
	overflow:
		compositor.server_time_monotonic = False;
		mb_log(X_WARNING, "meatball: the X server time does not seem to"
						  " be synchronized with the monotonic time.  Multiple"
						  " subsurfaces may be displayed at a reduced maximum"
						  " frame rate.\n");
	}
}

/**
 * Main work thread, intended to be called via pthreads.
 */
static void *
meatball_thread_init(void *__config_ptr__)
{
	struct meatball_config *config = (struct meatball_config *)__config_ptr__;
#if defined(HAVE_PTHREAD_SETNAME_NP_WITH_TID)
	pthread_setname_np(pthread_self(), "Meatball");
#elif defined(HAVE_PTHREAD_SETNAME_NP_WITHOUT_TID)
	pthread_setname_np("Meatball");
#endif

	/* Set up the log pipe before anything else so mb_log works. */
	FILE* mb_log_out = fdopen(config->log_pipe_write_fd, "w");
	if (!mb_log_out)
		return MEATBALL_FALSE;

	SetLoggerFile(mb_log_out);

	setlinebuf(mb_log_out);

	/* Set XAUTHORITY so XOpenDisplay can authenticate. */
	setenv("XAUTHORITY", config->auth_file, 1);

	/* Set the locale.  */
	setlocale(LC_ALL, "");

	/* Initialize Xlib threads. */
	if (!XInitThreads())
	{
		mb_log(X_ERROR, "meatball: Failed to initialize Xlib threading!\n");
		goto bail;
	}

	setenv("XAUTHORITY", config->auth_file, 1);

	Display *dpy = XOpenDisplay(config->display_name);
	if (!dpy)
	{
		mb_log(X_ERROR, "meatball: X11 display initialization failed\n");
		goto bail;
	}

	struct wl_display *wl_display = wl_display_create();
	if (!wl_display)
	{
		mb_log(X_ERROR, "meatball: Wayland display initialization failed\n");
		goto bail;
	}

	const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
	if (!runtime_dir || access(runtime_dir, W_OK) != 0)
	{
		setenv("XDG_RUNTIME_DIR", "/tmp", 1);
		runtime_dir = "/tmp";
	}

	const char *socket = wl_display_add_socket_auto(wl_display);
	if (!socket)
	{
		mb_log(X_ERROR, "meatball: Unable to add socket to Wayland display\n");
		goto bail;
	}

	/* Make the socket accessible to everyone. I would love
	 * to drop priviliges here but that is a clusterfuck with X. */
	char sock_path[128], lock_path[128];
	snprintf(sock_path, sizeof(sock_path), "/tmp/%s", socket);
	snprintf(lock_path, sizeof(lock_path), "/tmp/%s.lock", socket);
	chmod(sock_path, 0777);
	chmod(lock_path, 0666);

	/* Call XGetDefault with some dummy values to have the resource
	   database set up.  */
	XrmInitialize();
	XGetDefault(dpy, "dummy", "value");

	/* Setup the internal compositor data. */
	compositor.app_name = "Meatball";
	compositor.resource_name = "12to11";
	compositor.display = dpy;
	compositor.conn = XGetXCBConnection(dpy);
	compositor.wl_display = wl_display;
	compositor.wl_socket = socket;
	compositor.wl_event_loop = wl_display_get_event_loop(wl_display);

	/* Initialize server time tracking very early.  */
	InitTime();

	InitXErrors();
	SubcompositorInit();
	InitSelections();

	XLInitTimers();
	XLInitAtoms();

	/**
	 * Initialize renderers immediately after timers and
	 * atoms are setup.
	 */
	InitRenderers(config);
	SetRenderer(config);

	/* Intiialize all features. */
	for (int i = 0; i < ARRAY_SIZE(functions); i++) {
		const WaylandFeature feature = functions[i];
		if (!feature.func()) {
			mb_log(X_ERROR, "meatball: Initialization failure in %s\n", feature.name);
			goto bail;
		}
	}

	/* At this point we're ready. */
	mb_log(X_INFO, "meatball: Initialized\n");

	/* This has to come after the rest of the initialization.  */
	DetermineServerTime();
	XLRunCompositor();

bail:
	/* Exit the thread if we manage to exit the loop. */
	mb_flush();

	unlink(config->auth_file);
	free(config->auth_file);

	if (config)
		free(config);

	pthread_exit(NULL);
}

void meatball_version(unsigned long *version)
{
#ifndef MODULE_VERSION_NUMERIC
#define MODULE_VERSION_NUMERIC(maj, min, patch) \
	((((maj) & 0xFF) << 24) | (((min) & 0xFF) << 16) | (patch & 0xFFFF))
#endif
	*version = MODULE_VERSION_NUMERIC(1, 0, 4);
}

_MEATBALL_BOOL
meatball_initialize(struct meatball_config *config)
{
	if (!config)
		return MEATBALL_FALSE;

	meatball_thread_init(config);
	return MEATBALL_TRUE;
}