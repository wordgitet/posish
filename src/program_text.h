/* SPDX-License-Identifier: 0BSD */

/* posish - text-mode program execution helpers */

#ifndef POSISH_PROGRAM_TEXT_H
#define POSISH_PROGRAM_TEXT_H

#include "shell.h"

#include <stdbool.h>

typedef int (*program_text_command_runner)(struct shell_state *state,
                                           const char *source,
                                           bool allow_builtin);
typedef void (*program_text_child_runner)(struct shell_state *parent_state,
                                          const char *source);
typedef int (*program_text_async_runner)(struct shell_state *state,
                                         const char *source);
typedef bool (*program_text_try_ast_runner)(struct shell_state *state,
                                            const char *source,
                                            bool allow_builtin,
                                            int *status_out);
typedef bool (*program_text_flow_control_pred)(
    const struct shell_state *state);

struct program_text_hooks {
  program_text_command_runner run_command_atom;
  program_text_child_runner exec_child_command;
  program_text_async_runner run_async_list;
  program_text_try_ast_runner try_run_ast_compound_command;
  program_text_flow_control_pred has_pending_flow_control;
};

char *program_text_collapse_line_continuations(const char *source);
int program_text_execute(struct shell_state *state, const char *source,
                         const struct program_text_hooks *hooks);
int program_text_execute_internal(struct shell_state *state,
                                  const char *source, bool apply_aliases,
                                  const struct program_text_hooks *hooks);

#endif
