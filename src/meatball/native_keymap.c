/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "server.h"

#include <X11/extensions/XKM.h>
#include <errno.h>
#include <fcntl.h>
#include <inputstr.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xkbfile.h>
#include <xkbsrv.h>

#define KEYMAP_NAME "meatball-keymap"
#define KEYMAP_SIZE_LIMIT (1024 * 1024)
#define INVALID_KEYMAP_FD (-1)

int
mb_keymap_create (DeviceIntPtr keyboard, uint32_t *size)
{
    XkbComponentNamesRec names = { 0 };
    XkbDescRec description;
    char *text = NULL;
    size_t length = 0, written = 0;
    FILE *stream;
    int fd = INVALID_KEYMAP_FD;
    Bool valid;

    *size = 0;
    if (!keyboard || !keyboard->key || !keyboard->key->xkbInfo ||
        !keyboard->key->xkbInfo->desc)
        return INVALID_KEYMAP_FD;
    description = *keyboard->key->xkbInfo->desc;
    stream = open_memstream (&text, &length);
    if (!stream)
        return INVALID_KEYMAP_FD;
    valid = XkbWriteXKBKeymapForNames (stream, &names, &description,
                                       XkmKeymapRequired, XkmKeymapRequired);
    if (fclose (stream) != 0)
        valid = FALSE;
    free (names.keycodes);
    free (names.types);
    free (names.compat);
    free (names.symbols);
    free (names.geometry);
    if (!valid || !length || length >= KEYMAP_SIZE_LIMIT)
        goto out;
    fd = memfd_create (KEYMAP_NAME, MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd == INVALID_KEYMAP_FD)
        goto out;
    length++;
    while (written < length)
    {
        ssize_t count = write (fd, text + written, length - written);

        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            goto fail;
        written += count;
    }
    if (fcntl (fd, F_ADD_SEALS,
               F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL) < 0 ||
        lseek (fd, 0, SEEK_SET) < 0)
        goto fail;
    *size = length;
    goto out;

fail:
    close (fd);
    fd = INVALID_KEYMAP_FD;
out:
    free (text);
    return fd;
}
