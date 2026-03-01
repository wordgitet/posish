#include "arena.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *arena_xmalloc(size_t size) {
    void *ptr;

    if (size == 0) {
        size = 1;
    }

    ptr = malloc(size);
    if (ptr == NULL) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }
    return ptr;
}

void *arena_xrealloc(void *ptr, size_t size) {
    void *new_ptr;

    if (size == 0) {
        size = 1;
    }

    new_ptr = realloc(ptr, size);
    if (new_ptr == NULL) {
        perror("realloc");
        exit(EXIT_FAILURE);
    }
    return new_ptr;
}

char *arena_xstrdup(const char *s) {
    size_t len;
    char *copy;

    len = strlen(s);
    copy = arena_xmalloc(len + 1);
    memcpy(copy, s, len + 1);
    return copy;
}
