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
#include "meatball_log.h"

#include <unistd.h>

static FILE *mb_log_out = NULL;

void
mb_log (MessageType type, const char *format, ...)
{
    char type_char;
    switch (type)
    {
    case X_WARNING:
        type_char = 'W';
        break;
    case X_ERROR:
        type_char = 'E';
        break;
    case X_INFO:
        type_char = 'I';
        break;
    case X_DEBUG:
        type_char = 'D';
        break;
    default:
        type_char = 'I';
        break;
    }

    va_list args;
    va_start (args, format);
    fprintf (mb_log_out, "%c", type_char);
    vfprintf (mb_log_out, format, args);
    fflush (mb_log_out);
    va_end (args);
}

void
mb_crash_handler (int sig)
{
    fprintf (mb_log_out, "Emeatball: Crashed with signal %d\n", sig);
    fflush (mb_log_out);
    _exit (1);
}

void
mb_flush (void)
{
    fflush (mb_log_out);
}

void
SetLoggerFile (FILE *file)
{
    mb_log_out = file;
}