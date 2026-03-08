/* SPDX-License-Identifier: 0BSD */

/* posish - external command spawn helpers */

#ifndef POSISH_SPAWN_H
#define POSISH_SPAWN_H

#include "redir.h"
#include "shell.h"

typedef int (*spawn_body_runner)(struct shell_state *state,
                                 const void *payload);

int spawn_run_external_argv(struct shell_state *state, char *const argv[],
                            const struct redir_vec *redirs);
int exec_replace_with_utility(struct shell_state *state, const char *path,
                              char *const argv[]);
void spawn_exec_child_payload(struct shell_state *parent_state,
                              spawn_body_runner run_body,
                              const void *payload);
int spawn_run_subshell_payload(struct shell_state *parent_state,
                               const void *payload, const char *job_source,
                               spawn_body_runner run_body);
int spawn_run_async_payload(struct shell_state *state, const void *payload,
                            const char *job_source,
                            spawn_body_runner run_body);

#endif
