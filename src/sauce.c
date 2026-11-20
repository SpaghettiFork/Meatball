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

#include <xorg-server.h>

#include <errno.h>
#include <extension.h>
#include <extnsionst.h>
#include <opaque.h>
#include <os.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <xf86Module.h>
#include <xf86Optionstr.h>

#include "meatball/meatball.h"

#define SOCKET_PATH_OPTION "SocketPath"
#define SOCKET_MODE_OPTION "SocketMode"
#define RUNTIME_DIRECTORY_ENV "XDG_RUNTIME_DIR"
#define SOCKET_CHECK_INTERVAL_MS 1000

static MODULESETUPPROTO (sauce_setup);
static MODULETEARDOWNPROTO (sauce_shutdown);
static void sauce_check_socket (void *data, void *timeout);
static void sauce_wakeup (void *data, int result);

static Bool monitor_registered;
static Bool waiting_for_directory;
static CARD32 next_socket_check;
static Bool disabled;
static char *socket_path, *socket_directory;
static struct meatball_config config = {
    .socket_mode = MEATBALL_DEFAULT_SOCKET_MODE,
};

static XF86ModuleVersionInfo version = {
    .modname = "sauce",
    .vendor = "Spaghetti Fork",
    ._modinfo1_ = MODINFOSTRING1,
    ._modinfo2_ = MODINFOSTRING2,
    .xf86version = XORG_VERSION_CURRENT,
    .majorversion = MEATBALL_VERSION_MAJOR,
    .minorversion = MEATBALL_VERSION_MINOR,
    .patchlevel = MEATBALL_VERSION_PATCH,
    .abiclass = ABI_CLASS_EXTENSION,
    .abiversion = ABI_EXTENSION_VERSION,
};

_X_EXPORT XF86ModuleData sauceModuleData = {
    .vers = &version,
    .setup = sauce_setup,
    .teardown = sauce_shutdown,
};

static int
sauce_request (ClientPtr client)
{
    return BadRequest;
}

static void
sauce_reset (ExtensionEntry *extension)
{
    if (monitor_registered)
    {
        RemoveBlockAndWakeupHandlers (sauce_check_socket, sauce_wakeup, NULL);
        monitor_registered = FALSE;
    }
    waiting_for_directory = FALSE;
    meatball_shutdown ();
}

static void
sauce_wakeup (void *data, int result)
{
}

static void
sauce_check_socket (void *data, void *timeout)
{
    CARD32 now = GetTimeInMillis ();
    INT32 delay = next_socket_check - now;
    struct stat info;

    if (delay > 0)
    {
        AdjustWaitForDelay (timeout, delay);
        return;
    }
    if (meatball_running ())
    {
        if (lstat (socket_path, &info) == 0 || errno != ENOENT)
            goto wait;
        LogMessage (X_WARNING, "sauce: Wayland socket disappeared\n");
        meatball_shutdown ();
    }
    if (stat (socket_directory, &info) < 0 && errno == ENOENT)
    {
        if (!waiting_for_directory)
            LogMessage (X_INFO,
                        "sauce: Waiting for the Wayland socket directory\n");
        waiting_for_directory = TRUE;
        goto wait;
    }
    waiting_for_directory = FALSE;
    if (!meatball_initialize (&config))
    {
        /* The runtime directory can disappear between the check and bind. */
        if (stat (socket_directory, &info) < 0 && errno == ENOENT)
            goto wait;
        LogMessage (X_ERROR, "sauce: Meatball initialization failed\n");
        RemoveBlockAndWakeupHandlers (sauce_check_socket, sauce_wakeup, NULL);
        monitor_registered = FALSE;
        return;
    }

wait:
    next_socket_check = now + SOCKET_CHECK_INTERVAL_MS;
    AdjustWaitForDelay (timeout, SOCKET_CHECK_INTERVAL_MS);
}

static void
sauce_initialize (void)
{
    if (!AddExtension (MEATBALL_EXTENSION_NAME, 0, 0, sauce_request,
                       sauce_request, sauce_reset, StandardMinorOpcode))
    {
        LogMessage (X_ERROR, "sauce: Extension registration failed\n");
        return;
    }
    next_socket_check = GetTimeInMillis ();
    monitor_registered = RegisterBlockAndWakeupHandlers (sauce_check_socket,
                                                         sauce_wakeup, NULL);
    if (!monitor_registered)
        LogMessage (X_ERROR, "sauce: Startup handler registration failed\n");
}

static void *
sauce_setup (void *module, void *opts, int *errmaj, int *errmin)
{
    static Bool initialized;
    unsigned long abi;
    XF86OptionRec *option;
    const char *path = NULL;
    static const ExtensionModule extension = {
        .initFunc = sauce_initialize,
        .name = MEATBALL_EXTENSION_NAME,
        .disablePtr = &disabled,
    };

    if (initialized)
    {
        if (errmaj)
            *errmaj = LDR_ONCEONLY;
        return NULL;
    }
    meatball_version (&abi);
    if (GET_MODULE_MAJOR_VERSION (abi) != MEATBALL_VERSION_MAJOR ||
        GET_MODULE_MINOR_VERSION (abi) != MEATBALL_VERSION_MINOR)
    {
        if (errmaj)
            *errmaj = LDR_MISMATCH;
        return NULL;
    }
    for (option = opts; option; option = option->list.next)
    {
        if (!strcasecmp (option->opt_name, SOCKET_PATH_OPTION))
        {
            path = option->opt_val;
            option->opt_used = TRUE;
        }
        else if (!strcasecmp (option->opt_name, SOCKET_MODE_OPTION))
        {
            char *end;
            unsigned long mode;

            if (!option->opt_val || !*option->opt_val)
                goto invalid;
            errno = 0;
            mode = strtoul (option->opt_val, &end, 8);
            if (errno || *end || mode & ~MEATBALL_SOCKET_MODE_MASK)
                goto invalid;
            config.socket_mode = mode;
            option->opt_used = TRUE;
        }
    }
    if (path)
        socket_path = strdup (path);
    else
    {
        const char *directory = getenv (RUNTIME_DIRECTORY_ENV);

        if (!directory || directory[0] != '/')
            goto invalid;
        if (asprintf (&socket_path, "%s/%s%s", directory,
                      MEATBALL_SOCKET_PREFIX, display) < 0)
            socket_path = NULL;
    }
    if (!socket_path)
    {
        if (errmaj)
            *errmaj = LDR_NOMEM;
        return NULL;
    }
    if (socket_path[0] != '/')
        goto invalid;
    const char *separator = strrchr (socket_path, '/');
    size_t parent_length =
        separator == socket_path ? 1 : separator - socket_path;

    socket_directory = strndup (socket_path, parent_length);
    if (!socket_directory)
    {
        free (socket_path);
        socket_path = NULL;
        if (errmaj)
            *errmaj = LDR_NOMEM;
        return NULL;
    }
    config.socket_path = socket_path;
    LoadExtensionList (&extension, 1, FALSE);
    initialized = TRUE;
    return module;

invalid:
    free (socket_path);
    socket_path = NULL;
    LogMessage (X_ERROR,
                "sauce: Configure an absolute SocketPath or %s, and an octal "
                "SocketMode\n",
                RUNTIME_DIRECTORY_ENV);
    if (errmaj)
        *errmaj = LDR_MODSPECIFIC;
    return NULL;
}

static void
sauce_shutdown (void *data)
{
    sauce_reset (NULL);
    free (socket_directory);
    socket_directory = NULL;
    free (socket_path);
    socket_path = NULL;
    config.socket_path = NULL;
}
