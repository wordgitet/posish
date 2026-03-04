/* SPDX-License-Identifier: 0BSD */

/* posish - path utilities */

#include "path.h"

#include "arena.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

char *path_getcwd_alloc(void) {
    size_t size;
    char *buf;
    char *grown;
    char *result;

    size = 128;
    buf = malloc(size);
    if (buf == NULL) {
        return NULL;
    }
    for (;;) {
        if (getcwd(buf, size) != NULL) {
            result = arena_xstrdup(buf);
            free(buf);
            return result;
        }

        if (errno != ERANGE) {
            free(buf);
            return NULL;
        }
        if (size > SIZE_MAX / 2) {
            free(buf);
            errno = ERANGE;
            return NULL;
        }
        size *= 2;
        grown = realloc(buf, size);
        if (grown == NULL) {
            free(buf);
            return NULL;
        }
        buf = grown;
    }
}
