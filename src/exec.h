/* SPDX-License-Identifier: 0BSD */

/* posish - execution interface */

#ifndef POSISH_EXEC_H
#define POSISH_EXEC_H

#include "ast.h"
#include "shell.h"

int exec_run_program(struct shell_state *state, const struct ast_program *program);
void exec_prepare_signals_for_exec_child(const struct shell_state *state);

#endif
