/**
 * Spaghetti Display Server
 * Copyright (C) 2025-2026  SpaghettiFork
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

/**
 * Sauce.
 * The interposer between Spaghetti and Meatball.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include <xorg/dix.h>
#include <xorg/scrnintstr.h>
#include <xorg/xf86.h>
#include <xorg/xf86Module.h>
#include <xorg/xorg-server.h>
#define XCSECURITY 1
#include <X11/Xauth.h>
#include <xorg/os.h>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "meatball/meatball.h"

#define SAUCE_ABI_EXPECTED_MAJOR 1
#define SAUCE_ABI_EXPECTED_MINOR 0

#if defined(SP_GET_RENDER_FD)
#define SAUCE_HAS_RENDER_FD 1
#endif

static pid_t meatball_pid = 0;

static MODULESETUPPROTO (sauce_setup);
static MODULETEARDOWNPROTO (sauce_shutdown);

static XF86ModuleVersionInfo VersRec = {
    .modname = "sauce",
    .vendor = "Spaghetti Fork",
    ._modinfo1_ = MODINFOSTRING1,
    ._modinfo2_ = MODINFOSTRING2,
    .xf86version = XORG_VERSION_CURRENT,
    .majorversion = 1,
    .minorversion = 0,
    .patchlevel = 0,
    .abiclass = ABI_CLASS_EXTENSION,
    .abiversion = ABI_EXTENSION_VERSION,
};

_X_EXPORT XF86ModuleData sauceModuleData = { .vers = &VersRec,
                                             .setup = sauce_setup,
                                             .teardown = sauce_shutdown };

static void *
sauce_log_forwarder (void *arg)
{
    int fd = *(int *)arg;
    free (arg);

    char line[512];
    FILE *f = fdopen (fd, "r");
    if (!f)
        return NULL;

    while (fgets (line, sizeof (line), f))
    {
        MessageType type;
        switch (line[0])
        {
        case 'W':
            type = X_WARNING;
            break;
        case 'E':
            type = X_ERROR;
            break;
        case 'D':
            type = X_DEBUG;
            break;
        default:
            type = X_INFO;
            break;
        }
        LogMessage (type, "%s", line + 1);
    }

    fclose (f);
    return NULL;
}

#if 0
static ScrnInfoPtr
GetPrimaryScreen(void)
{
	for (int i = 0; i < screenInfo.numScreens; i++) {
		const ScreenPtr screen = screenInfo.screens[i];

		if (!screen)
			continue;

		if (!screen->isGPU)
			continue;

		/* Skip any secondary displays. */
		if (screen->current_primary)
			continue;

		return xf86ScreenToScrn(screen);
	}

	return NULL;
}
#endif

static void
sauce_fire (void *data, void *timeout)
{
    struct meatball_config *config = (struct meatball_config *)data;

    RemoveBlockAndWakeupHandlers (sauce_fire,
                                  (ServerWakeupHandlerProcPtr)NoopDDA, data);

    int log_pipe[2];
    if (pipe (log_pipe) != 0)
    {
        LogMessage (X_ERROR, "sauce: Failed to create log pipe\n");
        free (config->auth_file);
        free (config);
        return;
    }

    config->log_pipe_write_fd = log_pipe[1];

    pid_t pid = fork ();
    if (pid < 0)
    {
        LogMessage (X_ERROR, "sauce: fork() failed\n");
        close (log_pipe[0]);
        close (log_pipe[1]);
        free (config->auth_file);
        free (config);
        return;
    }

    if (pid == 0)
    {
        pid_t inner_pid = fork ();
        if (inner_pid < 0)
            _exit (1);

        if (inner_pid > 0)
        {
            _exit (0);
        }

        DIR *fddir = opendir ("/proc/self/fd");
        if (fddir)
        {
            int _dirfd = dirfd (fddir);
            struct dirent *de;
            while ((de = readdir (fddir)) != NULL)
            {
                int fd = atoi (de->d_name);
                if (fd < 3 || fd == config->log_pipe_write_fd || fd == _dirfd)
                    continue;
                close (fd);
            }
            closedir (fddir);
        }

        setsid ();
        meatball_initialize (config);
        _exit (0);
    }

    int status;
    waitpid (pid, &status, 0);

    /* Close the write end and start the log forwarder thread. */
    close (log_pipe[1]);

    int *read_fd = malloc (sizeof (int));
    if (read_fd)
    {
        *read_fd = log_pipe[0];
        pthread_t log_thread;
        if (pthread_create (&log_thread, NULL, sauce_log_forwarder, read_fd) ==
            0)
            pthread_detach (log_thread);
        else
            free (read_fd);
    }
}

static void *
sauce_setup (void *module, void *opts, int *errmaj, int *errmin)
{
    static Bool setupDone = FALSE;

    if (setupDone)
    {
        /* Allow only one instance of Meatball for now. */
        if (errmaj)
            *errmaj = LDR_ONCEONLY;
        return NULL;
    }

#if defined(SAUCE_ABI_EXPECTED_MAJOR) && defined(SAUCE_ABI_EXPECTED_MINOR)
    unsigned long version;
    meatball_version (&version);

    unsigned long major, minor;

    major = GET_MODULE_MAJOR_VERSION (version);
    minor = GET_MODULE_MINOR_VERSION (version);

    if (major != SAUCE_ABI_EXPECTED_MAJOR || minor != SAUCE_ABI_EXPECTED_MINOR)
    {
        /* ABI mismatch, likely something would break. */
        if (errmaj)
            *errmaj = LDR_MISMATCH;
        return NULL;
    }
#else
#warning "Sauce: ABI check unavailable, you might be in for a surprise!"
#endif

    struct meatball_config *config =
        calloc (1, sizeof (struct meatball_config));
    if (!config)
    {
        if (errmaj)
            *errmaj = LDR_NOMEM;
        return NULL;
    }

    /* Setup the display name. */
    snprintf (config->display_name, sizeof (config->display_name), ":%s",
              dixGetDisplayName (NULL));

    const char *cookie_type = "MIT-MAGIC-COOKIE-1";
    uint32_t name_len = strlen (cookie_type);
    uint32_t data_len = 0;

    char *cookie = NULL;
    if (GenerateAuthorization (name_len, cookie_type, 0, NULL, &data_len,
                               &cookie) == -1)
        goto bail;

    char auth_path[] = "/tmp/.meatball-auth-XXXXXX";
    FILE *af = fdopen (mkstemp (auth_path), "wb");
    if (!af)
        goto bail;

    const char *display_num = config->display_name + 1; /* skip ':' */

    Xauth entry = {
        .family = FamilyWild,
        .address_length = 0,
        .number = (char *)display_num,
        .number_length = strlen (display_num),
        .name = (char *)cookie_type,
        .name_length = name_len,
        .data = cookie,
        .data_length = data_len,
    };

    if (!XauWriteAuth (af, &entry))
    {
        fclose (af);
        goto bail;
    }

    fclose (af);
    chmod (auth_path, 0644);

    config->auth_file = strdup (auth_path);

    setupDone = TRUE;

    /* Software only for now. */
    config->meatball_flags = MB_FORCE_SOFTWARE_RENDERING;

    /* Because of how modules are loaded in Xorg, we need
     * to piggyback on a callback to correctly time the
     * Meatball initialization call otherwise the server
     * isn't ready for clients and it will fail to startup. */
    RegisterBlockAndWakeupHandlers (
        sauce_fire, (ServerWakeupHandlerProcPtr)NoopDDA, config);
    return module;
bail:
    if (errmaj)
        *errmaj = LDR_MODSPECIFIC;
    return NULL;
}

static void
sauce_shutdown (void *teardownData)
{
    meatball_shutdown ();
}
