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
    char *result;

    size = 128;
    buf = arena_alloc_in(NULL, size);
    for (;;) {
        if (getcwd(buf, size) != NULL) {
            result = arena_xstrdup(buf);
            arena_maybe_free(buf);
            return result;
        }

        if (errno != ERANGE) {
            arena_maybe_free(buf);
            return NULL;
        }
        if (size > SIZE_MAX / 2) {
            arena_maybe_free(buf);
            errno = ERANGE;
            return NULL;
        }
        size *= 2;
        buf = arena_realloc_in(NULL, buf, size);
    }
}
