/* SPDX-License-Identifier: 0BSD */

/* posish - AST execution helpers */

#ifndef POSISH_AST_EXEC_H
#define POSISH_AST_EXEC_H

#include "ast.h"
#include "shell.h"

#include <stdbool.h>

typedef int (*ast_exec_node_runner)(struct shell_state *state,
                                    const struct ast_node *node,
                                    bool allow_builtin);
typedef int (*ast_exec_body_runner)(struct shell_state *state, const char *body);
typedef void (*ast_exec_errexit_hook)(struct shell_state *state, int status);
typedef bool (*ast_exec_flow_control_pred)(const struct shell_state *state);

int ast_exec_run_group_with_redirections(struct shell_state *state,
                                         const char *body,
                                         const struct redir_vec *redirs,
                                         ast_exec_body_runner run_body);
int ast_exec_run_subshell_group(struct shell_state *state,
                                const struct ast_node *node,
                                ast_exec_body_runner run_subshell_body);
int ast_exec_run_brace_group(struct shell_state *state,
                             const struct ast_node *node,
                             ast_exec_body_runner run_brace_body);
int ast_exec_run_if(struct shell_state *state, const struct ast_node *node,
                    ast_exec_node_runner run_node,
                    ast_exec_errexit_hook maybe_trigger_errexit,
                    ast_exec_flow_control_pred has_pending_flow_control);
int ast_exec_run_loop(struct shell_state *state, const struct ast_node *node,
                      bool is_until, ast_exec_node_runner run_node,
                      ast_exec_errexit_hook maybe_trigger_errexit);

#endif
