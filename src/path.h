/* SPDX-License-Identifier: 0BSD */

/* posish - path interface */

#ifndef POSISH_PATH_H
#define POSISH_PATH_H

#include <stdbool.h>

struct shell_state;

char *path_getcwd_alloc(void);
void path_cache_invalidate(struct shell_state *state);
void path_cache_destroy(struct shell_state *state);
char *path_resolve_command(struct shell_state *state, const char *name,
                           bool use_standard_path);
bool path_resolves_command(struct shell_state *state, const char *name,
                           bool use_standard_path);
char *path_resolve_dot_script(struct shell_state *state, const char *name);

#endif
