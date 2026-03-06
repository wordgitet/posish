/* SPDX-License-Identifier: 0BSD */

/* posish - program entry point */

#include "shell.h"

#include "posish/version.h"
#include "arena.h"
#include "jobs.h"
#include "options.h"
#include "prompt.h"
#include "signals.h"
#include "vars.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(const char *argv0) {
    fprintf(stderr, "usage: %s [-c command] [script]\n", argv0);
}

static int set_initial_positional_params(struct shell_state *state, int argc,
                                         char **argv, int start_index) {
    int count;
    int i;

    if (start_index >= argc) {
        state->positional_params = NULL;
        state->positional_count = 0;
        return 0;
    }

    count = argc - start_index;
    state->positional_params =
        arena_alloc_in(&state->arena_perm,
                       sizeof(*state->positional_params) * (size_t)count);
    for (i = 0; i < count; i++) {
        state->positional_params[i] =
            arena_strdup_in(&state->arena_perm, argv[start_index + i]);
    }
    state->positional_count = (size_t)count;
    return 0;
}

int main(int argc, char **argv) {
    struct shell_state state;
    int status;
    int ret;
    int i;
    bool interactive_option_seen;
    bool force_interactive;
    bool run_interactive;
    bool read_stdin_script;
    bool refresh_signal_policy;
    const char *command;
    const char *special_0;
    const char *parent_interactive;

    shell_state_init(&state);
    options_init();
    vars_init();
    jobs_init();
    signals_init();

    {
        char ppid_buf[32];

        /* Keep PPID coherent for child shells used by the POSIX signal tests. */
        snprintf(ppid_buf, sizeof(ppid_buf), "%ld", (long)getppid());
        (void)setenv("PPID", ppid_buf, 1);
    }

    if (argc > 1 && strcmp(argv[1], "--version") == 0) {
        puts(POSISH_VERSION);
        shell_state_destroy(&state);
        return 0;
    }

    i = 1;
    interactive_option_seen = false;
    force_interactive = false;
    read_stdin_script = false;
    refresh_signal_policy = false;
    command = NULL;
    parent_interactive = getenv("POSISH_PARENT_INTERACTIVE");
    state.parent_was_interactive =
        parent_interactive != NULL && strcmp(parent_interactive, "1") == 0;
    while (i < argc) {
        size_t j;
        bool saw_c;

        if (strcmp(argv[i], "-i") == 0) {
            interactive_option_seen = true;
            force_interactive = true;
            i++;
            continue;
        }
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if ((argv[i][0] == '-' || argv[i][0] == '+') && argv[i][1] != '\0') {
            saw_c = false;
            for (j = 1; argv[i][j] != '\0'; j++) {
                bool enable;

                enable = argv[i][0] == '-';
                if (argv[i][j] == 'o') {
                    const char *name;

                    if (argv[i][j + 1] != '\0') {
                        name = argv[i] + j + 1;
                        j = strlen(argv[i]) - 1;
                    } else {
                        i++;
                        if (i >= argc) {
                            usage(argv[0]);
                            shell_state_destroy(&state);
                            return 2;
                        }
                        name = argv[i];
                    }
                    if (!options_apply_long(&state, name, enable,
                                            &refresh_signal_policy)) {
                        usage(argv[0]);
                        shell_state_destroy(&state);
                        return 2;
                    }
                    if (strcmp(name, "interactive") == 0) {
                        interactive_option_seen = true;
                        force_interactive = enable;
                        state.explicit_non_interactive = !enable;
                    }
                    if (strcmp(name, "monitor") == 0) {
                        refresh_signal_policy = true;
                    }
                    break;
                } else if (argv[i][j] == 's') {
                    read_stdin_script = enable;
                } else if (argv[i][0] == '-' && argv[i][j] == 'c') {
                    saw_c = true;
                } else if (options_apply_short(&state, argv[i][j], enable,
                                               &refresh_signal_policy)) {
                    if (argv[i][j] == 'i') {
                        interactive_option_seen = true;
                        force_interactive = enable;
                        state.explicit_non_interactive = !enable;
                    }
                } else {
                    usage(argv[0]);
                    shell_state_destroy(&state);
                    return 2;
                }
                if (argv[i][j] == 'i') {
                    interactive_option_seen = true;
                    force_interactive = enable;
                    state.explicit_non_interactive = !enable;
                }
            }
            i++;
            if (saw_c) {
                if (i >= argc) {
                    usage(argv[0]);
                    return 2;
                }
                command = argv[i];
                i++;
                if ((strcmp(command, "-") == 0 || strcmp(command, "--") == 0) &&
                    i < argc) {
                    /* POSIX: leading -/-- after -c is ignored as command name. */
                    command = argv[i];
                    i++;
                }
                break;
            }
            continue;
        }
        break;
    }

    if (interactive_option_seen) {
        run_interactive = force_interactive;
    } else {
        run_interactive = (command == NULL && i >= argc && isatty(STDIN_FILENO));
    }

    if (command == NULL && !read_stdin_script && i < argc &&
        strcmp(argv[i], "-") == 0) {
        /*
         * A lone "-" operand is ignored and stdin is read as the script
         * source, matching the startup test-suite contract.
         */
        read_stdin_script = true;
        i++;
    }

    /*
     * Keep special parameter $0 available through expansion paths.
     * -c may supply an explicit command name operand as $0.
     */
    special_0 = argv[0];
    if (command != NULL && i < argc) {
        special_0 = argv[i];
    } else if (!read_stdin_script && i < argc) {
        special_0 = argv[i];
    }
    (void)setenv("0", special_0, 1);

    state.interactive = run_interactive;
    prompt_init_defaults(&state, argv[0]);
    (void)refresh_signal_policy;
    shell_refresh_signal_policy(&state);

    if (command != NULL) {
        (void)set_initial_positional_params(&state, argc, argv, i + 1);
    } else if (read_stdin_script) {
        (void)set_initial_positional_params(&state, argc, argv, i);
    } else if (i < argc) {
        (void)set_initial_positional_params(&state, argc, argv, i + 1);
    }

    if (command != NULL) {
        status = shell_run_command(&state, command);
    } else if (read_stdin_script) {
        status = shell_run_stream(&state, stdin, run_interactive);
    } else if (i < argc) {
        status = shell_run_file(&state, argv[i]);
    } else {
        status = shell_run_stream(&state, stdin, run_interactive);
    }

    /* Drain late-arriving pending signals before shutdown/final EXIT trap. */
    shell_run_pending_traps(&state);
    shell_run_exit_trap(&state);

    if (state.should_exit) {
        ret = state.exit_status;
    } else {
        ret = status;
    }

    jobs_destroy();
    shell_state_destroy(&state);
    return ret;
}
