/*
** Copyright 2005-2016  Solarflare Communications Inc.
**                      7505 Irvine Center Drive, Irvine, CA 92618, USA
** Copyright 2002-2005  Level 5 Networks Inc.
**
** This program is free software; you can redistribute it and/or modify it
** under the terms of version 2 of the GNU General Public License as
** published by the Free Software Foundation.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
*/

/*
 * Copyright (c) 2009-2014 Petri Lehtinen <petri@digip.org>
 *
 * Jansson is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See LICENSE for details.
 */

#ifndef UTIL_H
#define UTIL_H

#ifdef HAVE_CONFIG_H
#include <jansson_private_config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#if HAVE_LOCALE_H
#include <locale.h>
#endif

#include <jansson.h>

#define failhdr fprintf(stderr, "%s:%s:%d: ", __FILE__, __FUNCTION__, __LINE__)

#define fail(msg)                                                \
    do {                                                         \
        failhdr;                                                 \
        fprintf(stderr, "%s\n", msg);                            \
        exit(1);                                                 \
    } while(0)

/* Assumes json_error_t error */
#define check_error(text_, source_, line_, column_, position_)          \
    do {                                                                \
        if(strcmp(error.text, text_) != 0) {                            \
            failhdr;                                                    \
            fprintf(stderr, "text: \"%s\" != \"%s\"\n", error.text, text_); \
            exit(1);                                                    \
        }                                                               \
        if(strcmp(error.source, source_) != 0) {                        \
            failhdr;                                                    \
                                                                        \
            fprintf(stderr, "source: \"%s\" != \"%s\"\n", error.source, source_); \
            exit(1);                                                    \
        }                                                               \
        if(error.line != line_) {                                       \
            failhdr;                                                    \
            fprintf(stderr, "line: %d != %d\n", error.line, line_);     \
            exit(1);                                                    \
        }                                                               \
        if(error.column != column_) {                                   \
            failhdr;                                                    \
            fprintf(stderr, "column: %d != %d\n", error.column, column_); \
            exit(1);                                                    \
        }                                                               \
        if(error.position != position_) {                               \
            failhdr;                                                    \
            fprintf(stderr, "position: %d != %d\n", error.position, position_); \
            exit(1);                                                    \
        }                                                               \
    } while(0)


static void run_tests();

int main() {
#ifdef HAVE_SETLOCALE
    setlocale(LC_ALL, "");
#endif
    run_tests();
    return 0;
}

#endif
