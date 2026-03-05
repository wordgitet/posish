/* SPDX-License-Identifier: 0BSD */

/* posish - alias expansion interface */

#ifndef POSISH_ALIAS_H
#define POSISH_ALIAS_H

#include <stdbool.h>
#include <stddef.h>

struct shell_state;

/* Look up an alias value by name (arena-allocated copy, or NULL). */
char *alias_lookup_dup(const char *name);

/* Returns true if word looks like VAR=value. */
bool alias_is_assignment_word(const char *word);

/*
 * Rewrite aliases in a shell snippet.
 * Returns 0 on success.  *out is arena-allocated.
 * *changed is set to true if any alias was expanded.
 */
int alias_rewrite_snippet(struct shell_state *state, const char *text,
                          char **out, bool *changed);

#endif
