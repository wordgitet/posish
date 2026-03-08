/* SPDX-License-Identifier: 0BSD */

/* posish - alias expansion interface */

#ifndef POSISH_ALIAS_H
#define POSISH_ALIAS_H

#include <stdbool.h>
#include <stddef.h>

struct shell_state;

typedef bool (*alias_visit_fn)(const char *name, const char *value,
                               void *user_data);

void aliases_init(struct shell_state *state);
void aliases_destroy(struct shell_state *state);
const char *alias_lookup(const struct shell_state *state, const char *name);
char *alias_lookup_dup(const struct shell_state *state, const char *name);
int alias_set(struct shell_state *state, const char *name, const char *value);
int alias_unset(struct shell_state *state, const char *name);
void alias_clear(struct shell_state *state);
void alias_for_each(const struct shell_state *state, alias_visit_fn visit,
                    void *user_data);

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
