/* SPDX-License-Identifier: 0BSD */

/* loginexec - exec a program with login-shell argv[0] */

#define _POSIX_C_SOURCE 200809L

#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *base_name(const char *path) {
    const char *slash;

    slash = strrchr(path, '/');
    if (slash == NULL || slash[1] == '\0') {
        return path;
    }
    return slash + 1;
}

int main(int argc, char **argv) {
    char **child_argv;
    const char *base;
    size_t base_len;

    if (argc < 2) {
        fprintf(stderr, "usage: %s program [args...]\n", argv[0]);
        return 2;
    }

    child_argv = calloc((size_t)argc, sizeof(*child_argv));
    if (child_argv == NULL) {
        perror("calloc");
        return 1;
    }

    base = base_name(argv[1]);
    base_len = strlen(base);
    child_argv[0] = malloc(base_len + 2u);
    if (child_argv[0] == NULL) {
        perror("malloc");
        free(child_argv);
        return 1;
    }

    child_argv[0][0] = '-';
    memcpy(child_argv[0] + 1, base, base_len + 1u);
    for (int i = 2; i < argc; i++) {
        child_argv[i - 1] = argv[i];
    }
    child_argv[argc - 1] = NULL;

    execv(argv[1], child_argv);
    perror(argv[1]);
    free(child_argv[0]);
    free(child_argv);
    return 126;
}
