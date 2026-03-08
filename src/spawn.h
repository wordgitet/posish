/* SPDX-License-Identifier: 0BSD */

/* posish - external command spawn helpers */

#ifndef POSISH_SPAWN_H
#define POSISH_SPAWN_H

#include "redir.h"
#include "shell.h"

int spawn_run_external_argv(struct shell_state *state, char *const argv[],
                            const struct redir_vec *redirs);
int exec_replace_with_utility(struct shell_state *state, const char *path,
                              char *const argv[]);

#endif
