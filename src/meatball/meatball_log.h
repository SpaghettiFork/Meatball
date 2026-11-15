/**
 * Spaghetti Display Server
 * Copyright (C) 2026  SpaghettiFork
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
#ifndef _MEATBALL_LOG_H_
#define _MEATBALL_LOG_H_

#include <stdarg.h>
#include <stdio.h>

typedef enum
{
    X_PROBED,          /* Value was probed */
    X_CONFIG,          /* Value was given in the config file */
    X_DEFAULT,         /* Value is a default */
    X_CMDLINE,         /* Value was given on the command line */
    X_NOTICE,          /* Notice */
    X_ERROR,           /* Error message */
    X_WARNING,         /* Warning message */
    X_INFO,            /* Informational message */
    X_NONE,            /* No prefix */
    X_NOT_IMPLEMENTED, /* Not implemented */
    X_DEBUG,           /* Debug message */
    X_UNKNOWN = -1     /* unknown -- this must always be last */
} MessageType;

void mb_log (MessageType type, const char *format, ...);

extern void mb_flush (void);

extern void mb_crash_handler (int);

void SetLoggerFile (FILE *file);

#endif // _MEATBALL_LOG_H_