/* SPDX-License-Identifier: 0BSD */

/* posish - shell function table */

#ifndef POSISH_FUNCTIONS_H
#define POSISH_FUNCTIONS_H

#include "shell.h"

#include <stdbool.h>

void functions_init(struct shell_state *state);
void functions_destroy(struct shell_state *state);

bool functions_has(const struct shell_state *state, const char *name);
const struct shell_function *functions_get(const struct shell_state *state,
                                           const char *name);
struct shell_function *functions_get_mut(struct shell_state *state,
                                         const char *name);
const struct ast_program *functions_get_cached_program(struct shell_state *state,
                                                       struct shell_function *function);
int functions_set(struct shell_state *state, const char *name, const char *body,
                  const struct redir_vec *redirs);
int functions_remove(struct shell_state *state, const char *name);

#endif
