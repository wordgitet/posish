#include "shell.h"

#include "posish/version.h"
#include "jobs.h"
#include "options.h"
#include "signals.h"
#include "vars.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(const char *argv0) {
    fprintf(stderr, "usage: %s [-c command] [script]\n", argv0);
}

int main(int argc, char **argv) {
    struct shell_state state;
    int status;
    int ret;
    int i;
    bool interactive_option_seen;
    bool force_interactive;
    bool run_interactive;
    const char *command;
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
                if (argv[i][j] == 'e') {
                    state.errexit = argv[i][0] == '-';
                } else if (argv[i][j] == 'i') {
                    interactive_option_seen = true;
                    force_interactive = argv[i][0] == '-';
                    state.explicit_non_interactive = argv[i][0] == '+';
                } else if (argv[i][j] == 'm') {
                    state.monitor_mode = argv[i][0] == '-';
                } else if (argv[i][j] == 'v') {
                    state.verbose = argv[i][0] == '-';
                } else if (argv[i][0] == '-' && argv[i][j] == 'c') {
                    saw_c = true;
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
    state.interactive = run_interactive;
    shell_refresh_signal_policy(&state);

    if (command != NULL) {
        status = shell_run_command(&state, command);
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
