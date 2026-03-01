/* SPDX-License-Identifier: 0BSD */

/* posish - here-document interface */

#ifndef POSISH_HEREDOC_COMMAND_H
#define POSISH_HEREDOC_COMMAND_H

#include <stdbool.h>
#include <stddef.h>

#include "shell.h"

typedef int (*heredoc_command_runner_fn)(struct shell_state *state,
                                         const char *source);

int maybe_execute_heredoc_command(struct shell_state *state, const char *command,
                                  const char *source, size_t body_pos,
                                  size_t *new_pos_out, bool *handled,
                                  int *status_out,
                                  heredoc_command_runner_fn runner,
                                  bool preserve_tempfiles);

#endif
