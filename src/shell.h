/* SPDX-License-Identifier: 0BSD */

/* posish - shell state and APIs */

#ifndef POSISH_SHELL_H
#define POSISH_SHELL_H

#include "arena.h"

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/types.h>

struct shell_function {
    char *name;
    char *body;
};

struct shell_state {
    int last_status;
    int last_cmdsub_status;
    bool cmdsub_performed;
    pid_t shell_pid;
    bool errexit;
    bool errexit_ignored;
    bool should_exit;
    int exit_status;
    int last_handled_signal;
    bool interactive;
    bool explicit_non_interactive;
    bool parent_was_interactive;
    bool monitor_mode;
    bool allexport;
    bool notify;
    bool noclobber;
    bool noglob;
    bool hashondef;
    bool noexec;
    bool nounset;
    bool verbose;
    bool xtrace;
    bool pipefail;
    bool ignoreeof;
    bool in_async_context;
    bool main_context;
    bool in_command_builtin;
    pid_t last_async_pid;
    int break_levels;
    int continue_levels;
    int loop_depth;
    bool return_requested;
    int return_status;
    int function_depth;
    int dot_depth;
    char *exit_trap;
    bool running_exit_trap;
    bool running_signal_trap;
    bool trap_entry_status_valid;
    int trap_entry_status;
    char *signal_traps[NSIG];
    bool signal_cleared[NSIG];
    char **readonly_names;
    size_t readonly_count;
    struct shell_function *functions;
    size_t function_count;
    char **unexported_names;
    size_t unexported_count;
    char **positional_params;
    size_t positional_count;
    struct arena arena_perm;
    struct arena arena_script;
    struct arena arena_cmd;
};

void shell_state_init(struct shell_state *state);
void shell_state_destroy(struct shell_state *state);
void shell_refresh_signal_policy(struct shell_state *state);
void shell_run_exit_trap(struct shell_state *state);
void shell_run_pending_traps(struct shell_state *state);
int shell_run_command(struct shell_state *state, const char *command);
int shell_run_file(struct shell_state *state, const char *path);
int shell_run_stream(struct shell_state *state, FILE *stream, bool interactive);
int shell_needs_more_input_text_mode(const char *buf, size_t len,
                                     bool include_heredoc);
int shell_needs_more_input_text(const char *buf, size_t len);
bool shell_position_in_comment(const char *buf, size_t len, size_t pos);

#endif
