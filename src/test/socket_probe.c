/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "probe.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define SOCKET_PATH_ENV "MEATBALL_TEST_SOCKET"
#define TARGET_PATH_ENV "MEATBALL_TEST_TARGET"
#define BACKUP_PATH_ENV "MEATBALL_TEST_BACKUP"

extern int __real_chmod (const char *path, mode_t mode);
extern int __real_fchownat (int dirfd, const char *path, uid_t owner,
                            gid_t group, int flags);
extern int __wrap_chmod (const char *path, mode_t mode);
extern int __wrap_fchownat (int dirfd, const char *path, uid_t owner,
                            gid_t group, int flags);

static Bool replaced;

static void
replace_socket (void)
{
    if (replaced)
        return;
    assert (rename (getenv (SOCKET_PATH_ENV), getenv (BACKUP_PATH_ENV)) == 0);
    assert (symlink (getenv (TARGET_PATH_ENV), getenv (SOCKET_PATH_ENV)) == 0);
    replaced = TRUE;
}

int
__wrap_chmod (const char *path, mode_t mode)
{
    replace_socket ();
    return __real_chmod (path, mode);
}

int
__wrap_fchownat (int dirfd, const char *path, uid_t owner, gid_t group,
                 int flags)
{
    replace_socket ();
    return __real_fchownat (dirfd, path, owner, group, flags);
}

void
mb_probe_start (void)
{
    assert (replaced);
}

void
mb_probe_stop (void)
{
    replaced = FALSE;
}
