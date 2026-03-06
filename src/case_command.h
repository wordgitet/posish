/* SPDX-License-Identifier: 0BSD */

/* posish - case command interface */

#ifndef POSISH_CASE_COMMAND_H
#define POSISH_CASE_COMMAND_H

#include "shell.h"

#include <stdbool.h>
#include <stddef.h>

struct ast_case_clause;
struct ast_node;

typedef int (*case_command_runner_fn)(struct shell_state *state,
                                      const char *source);
typedef int (*case_command_ast_runner_fn)(struct shell_state *state,
                                          const struct ast_node *node);

bool try_execute_case_command(struct shell_state *state, const char *source,
                              int *status_out, case_command_runner_fn runner);
int execute_structured_case_command(struct shell_state *state,
                                    const char *word_expr,
                                    const struct ast_case_clause *clauses,
                                    size_t clause_count,
                                    case_command_runner_fn source_runner,
                                    case_command_ast_runner_fn ast_runner);

#endif
