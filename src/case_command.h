#ifndef POSISH_CASE_COMMAND_H
#define POSISH_CASE_COMMAND_H

#include <stdbool.h>

#include "shell.h"

typedef int (*case_command_runner_fn)(struct shell_state *state,
                                      const char *source);

bool try_execute_case_command(struct shell_state *state, const char *source,
                              int *status_out, case_command_runner_fn runner);

#endif
