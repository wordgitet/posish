/* SPDX-License-Identifier: 0BSD */

/* posish - command substitution helpers */

#ifndef POSISH_COMMAND_SUBST_H
#define POSISH_COMMAND_SUBST_H

#include "shell.h"

#include <stdbool.h>
#include <stddef.h>

bool command_subst_find_close(const char *in, size_t start, size_t *close_out);
size_t command_subst_skip_dollar_paren(const char *token, size_t pos);
size_t command_subst_skip_backtick(const char *token, size_t pos);
int command_subst_run(struct shell_state *state, const char *cmd,
                      char **out_value, int *status_out);
char *command_subst_normalize_backquote(const char *raw);

#endif
