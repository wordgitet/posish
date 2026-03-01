/* SPDX-License-Identifier: 0BSD */

/* posish - input helpers */

#include "input.h"

#include "arena.h"

#include <stdio.h>

int input_read_file(const char *path, char **out_contents) {
    FILE *fp;
    char *buf;
    size_t len;
    size_t cap;
    int c;

    fp = fopen(path, "r");
    if (fp == NULL) {
        return -1;
    }

    cap = 4096;
    len = 0;
    buf = arena_xmalloc(cap);

    while ((c = fgetc(fp)) != EOF) {
        if (len + 1 >= cap) {
            cap *= 2;
            buf = arena_xrealloc(buf, cap);
        }
        buf[len++] = (char)c;
    }

    buf[len] = '\0';

    if (ferror(fp)) {
        fclose(fp);
        return -1;
    }

    fclose(fp);
    *out_contents = buf;
    return 0;
}
