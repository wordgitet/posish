#ifndef POSISH_SHELL_H
#define POSISH_SHELL_H

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
    bool should_exit;
    int exit_status;
    int last_handled_signal;
    bool interactive;
    bool explicit_non_interactive;
    bool parent_was_interactive;
    bool monitor_mode;
    bool in_async_context;
    bool main_context;
    pid_t last_async_pid;
    char *exit_trap;
    bool running_exit_trap;
    bool running_signal_trap;
    char *signal_traps[NSIG];
    bool signal_cleared[NSIG];
    char **readonly_names;
    size_t readonly_count;
    struct shell_function *functions;
    size_t function_count;
    char **positional_params;
    size_t positional_count;
};

void shell_state_init(struct shell_state *state);
void shell_state_destroy(struct shell_state *state);
void shell_refresh_signal_policy(struct shell_state *state);
void shell_run_exit_trap(struct shell_state *state);
void shell_run_pending_traps(struct shell_state *state);
int shell_run_command(struct shell_state *state, const char *command);
int shell_run_file(struct shell_state *state, const char *path);
int shell_run_stream(struct shell_state *state, FILE *stream, bool interactive);

#endif
