/* SPDX-License-Identifier: 0BSD */

/* posish - execution interface */

#ifndef POSISH_EXEC_H
#define POSISH_EXEC_H

#include "ast.h"
#include "shell.h"

int exec_run_program(struct shell_state *state, const struct ast_program *program);
char *exec_alias_expand_preview(struct shell_state *state, const char *source);
bool exec_alias_preview_needs_more(const char *preview);
bool exec_noexec_allows_set_toggle(const char *source);

#endif
