/* SPDX-License-Identifier: 0BSD */

/* posish - path utilities */

#include "path.h"

#include "arena.h"

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

char *path_getcwd_alloc(void) {
    size_t size;
    char *buf;

    size = 128;
    for (;;) {
        buf = arena_alloc_in(NULL, size);
        if (getcwd(buf, size) != NULL) {
            return buf;
        }

        arena_maybe_free(buf);
        if (errno != ERANGE) {
            return NULL;
        }
        size *= 2;
    }
}
