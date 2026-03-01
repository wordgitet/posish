#include "builtins/builtin.h"

#include "error.h"
#include "jobs.h"
#include "path.h"

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define POSISH_ALIAS_ENV_PREFIX "POSISH_ALIAS_"

static int wait_status_to_shell_status(int status) {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    if (WIFSTOPPED(status)) {
        return 128 + WSTOPSIG(status);
    }
    return 1;
}

static int builtin_cd(char *const argv[]) {
    const char *target;
    char *old_pwd;
    char *new_pwd;

    if (argv[2] != NULL) {
        posish_errorf("cd: too many arguments");
        return 1;
    }

    target = argv[1];
    if (target == NULL) {
        target = getenv("HOME");
        if (target == NULL) {
            posish_errorf("cd: HOME is not set");
            return 1;
        }
    }

    old_pwd = path_getcwd_alloc();
    if (chdir(target) != 0) {
        free(old_pwd);
        perror("cd");
        return 1;
    }

    if (old_pwd != NULL) {
        (void)setenv("OLDPWD", old_pwd, 1);
    }

    new_pwd = path_getcwd_alloc();
    if (new_pwd != NULL) {
        (void)setenv("PWD", new_pwd, 1);
    }

    free(old_pwd);
    free(new_pwd);
    return 0;
}

static int run_utility(char *const argv[]) {
    int status;
    pid_t pid;

    pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        execvp(argv[0], argv);
        perror(argv[0]);
        _exit(127);
    }

    for (;;) {
        if (waitpid(pid, &status, 0) < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("waitpid");
            return 1;
        }
        break;
    }

    return wait_status_to_shell_status(status);
}

static int builtin_test(char *const argv[]) {
    if (strcmp(argv[0], "[") == 0) {
        size_t argc;
        size_t i;
        char **test_argv;
        int status;

        argc = 0;
        while (argv[argc] != NULL) {
            argc++;
        }
        if (argc < 2 || strcmp(argv[argc - 1], "]") != 0) {
            posish_errorf("[: missing closing ]");
            return 2;
        }

        test_argv = calloc(argc, sizeof(*test_argv));
        if (test_argv == NULL) {
            perror("calloc");
            return 1;
        }
        test_argv[0] = "test";
        for (i = 1; i + 1 < argc; i++) {
            test_argv[i] = argv[i];
        }
        test_argv[argc - 1] = NULL;

        status = run_utility(test_argv);
        free(test_argv);
        return status;
    }

    return run_utility(argv);
}

static int builtin_echoraw(char *const argv[]) {
    size_t i;

    for (i = 1; argv[i] != NULL; i++) {
        if (i > 1) {
            putchar(' ');
        }
        fputs(argv[i], stdout);
    }
    putchar('\n');
    return 0;
}

static int builtin_bracket(char *const argv[]) {
    size_t i;

    for (i = 1; argv[i] != NULL; i++) {
        printf("[%s]", argv[i]);
    }
    putchar('\n');
    return 0;
}

static char *pid_to_string(pid_t pid) {
    char buf[32];
    int len;
    char *out;

    len = snprintf(buf, sizeof(buf), "%ld", (long)pid);
    if (len < 0 || (size_t)len >= sizeof(buf)) {
        return NULL;
    }

    out = malloc((size_t)len + 1);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, buf, (size_t)len + 1);
    return out;
}

static int builtin_kill(char *const argv[]) {
    size_t argc;
    size_t i;
    char **converted;
    bool list_mode;
    int status;

    argc = 0;
    while (argv[argc] != NULL) {
        argc++;
    }

    converted = calloc(argc + 1, sizeof(*converted));
    if (converted == NULL) {
        perror("calloc");
        return 1;
    }

    list_mode = false;
    for (i = 0; i < argc; i++) {
        if (i > 0 &&
            (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--list") == 0 ||
             strncmp(argv[i], "--list=", 7) == 0)) {
            list_mode = true;
            converted[i] = argv[i];
            continue;
        }

        if (list_mode && argv[i][0] != '%' &&
            isdigit((unsigned char)argv[i][0])) {
            char *end;
            long value;

            errno = 0;
            value = strtol(argv[i], &end, 10);
            if (errno == 0 && end != argv[i] && *end == '\0' && value > 128) {
                char *status_text;

                status_text = pid_to_string((pid_t)(value - 128));
                if (status_text == NULL) {
                    perror("malloc");
                    status = 1;
                    goto done;
                }
                converted[i] = status_text;
                continue;
            }
        }

        if (argv[i][0] == '%') {
            pid_t pid;
            char *pid_text;

            pid = jobs_resolve_spec(argv[i]);
            if (pid <= 0) {
                posish_errorf("kill: no such job: %s", argv[i]);
                status = 1;
                goto done;
            }

            pid_text = pid_to_string(pid);
            if (pid_text == NULL) {
                perror("malloc");
                status = 1;
                goto done;
            }
            converted[i] = pid_text;
        } else {
            converted[i] = argv[i];
        }
    }
    converted[argc] = NULL;

    status = run_utility(converted);

done:
    for (i = 0; i < argc; i++) {
        if (converted[i] != NULL && converted[i] != argv[i]) {
            free(converted[i]);
        }
    }
    free(converted);
    return status;
}

static bool alias_name_valid(const char *name) {
    size_t i;

    if (name == NULL || name[0] == '\0') {
        return false;
    }
    if (!(isalpha((unsigned char)name[0]) || name[0] == '_')) {
        return false;
    }
    for (i = 1; name[i] != '\0'; i++) {
        if (!(isalnum((unsigned char)name[i]) || name[i] == '_')) {
            return false;
        }
    }
    return true;
}

static char *alias_env_key(const char *name) {
    size_t plen;
    size_t nlen;
    char *key;

    plen = strlen(POSISH_ALIAS_ENV_PREFIX);
    nlen = strlen(name);
    key = malloc(plen + nlen + 1);
    if (key == NULL) {
        perror("malloc");
        return NULL;
    }
    memcpy(key, POSISH_ALIAS_ENV_PREFIX, plen);
    memcpy(key + plen, name, nlen + 1);
    return key;
}

static int builtin_alias(char *const argv[]) {
    int status;
    size_t i;

    if (argv[1] == NULL) {
        return 0;
    }

    status = 0;
    for (i = 1; argv[i] != NULL; i++) {
        char *eq;
        char *name;
        char *key;
        const char *value;

        eq = strchr(argv[i], '=');
        if (eq == NULL) {
            status = 1;
            continue;
        }

        name = malloc((size_t)(eq - argv[i]) + 1);
        if (name == NULL) {
            perror("malloc");
            return 1;
        }
        memcpy(name, argv[i], (size_t)(eq - argv[i]));
        name[eq - argv[i]] = '\0';
        value = eq + 1;

        if (!alias_name_valid(name)) {
            posish_errorf("alias: invalid name: %s", name);
            free(name);
            status = 1;
            continue;
        }

        key = alias_env_key(name);
        if (key == NULL) {
            free(name);
            return 1;
        }
        if (setenv(key, value, 1) != 0) {
            perror("setenv");
            free(key);
            free(name);
            return 1;
        }

        free(key);
        free(name);
    }

    return status;
}

static int builtin_unalias(char *const argv[]) {
    int status;
    size_t i;

    if (argv[1] == NULL) {
        posish_errorf("unalias: missing operand");
        return 1;
    }

    status = 0;
    for (i = 1; argv[i] != NULL; i++) {
        char *key;

        if (!alias_name_valid(argv[i])) {
            posish_errorf("unalias: invalid name: %s", argv[i]);
            status = 1;
            continue;
        }

        key = alias_env_key(argv[i]);
        if (key == NULL) {
            return 1;
        }
        if (unsetenv(key) != 0) {
            perror("unsetenv");
            free(key);
            return 1;
        }
        free(key);
    }
    return status;
}

static int wait_for_pid(struct shell_state *state, pid_t pid, int *status_out,
                        int *interrupted_signal_out) {
    int status;

    for (;;) {
        pid_t w;
        int previous_signal;

        previous_signal = state->last_handled_signal;
        w = waitpid(pid, &status, 0);
        if (w < 0) {
            if (errno == EINTR) {
                shell_run_pending_traps(state);
                if (interrupted_signal_out != NULL &&
                    state->last_handled_signal != previous_signal) {
                    *interrupted_signal_out = state->last_handled_signal;
                    errno = EINTR;
                    return -1;
                }
                continue;
            }
            return -1;
        }
        *status_out = status;
        return 0;
    }
}

static int parse_wait_operand(const char *text, pid_t *pid_out) {
    char *end;
    long n;

    if (text[0] == '%') {
        pid_t pid;

        pid = jobs_resolve_spec(text);
        if (pid <= 0) {
            errno = ESRCH;
            return -1;
        }
        *pid_out = pid;
        return 0;
    }

    errno = 0;
    n = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || n <= 0) {
        errno = EINVAL;
        return -1;
    }
    *pid_out = (pid_t)n;
    return 0;
}

static int builtin_wait(struct shell_state *state, char *const argv[]) {
    int last_status;
    size_t i;

    last_status = 0;
    if (argv[1] == NULL) {
        for (;;) {
            int status;
            pid_t w;
            int previous_signal;

            previous_signal = state->last_handled_signal;
            w = waitpid(-1, &status, 0);
            if (w < 0) {
                if (errno == EINTR) {
                    shell_run_pending_traps(state);
                    if (state->last_handled_signal != previous_signal) {
                        return 128 + state->last_handled_signal;
                    }
                    continue;
                }
                if (errno == ECHILD) {
                    break;
                }
                perror("wait");
                return 1;
            }

            jobs_forget(w);
            if (w == state->last_async_pid) {
                state->last_async_pid = -1;
            }
            last_status = wait_status_to_shell_status(status);
        }

        (void)last_status;
        return 0;
    }

    for (i = 1; argv[i] != NULL; i++) {
        pid_t pid;
        int interrupted_signal;
        int status;

        if (parse_wait_operand(argv[i], &pid) != 0) {
            if (errno == ESRCH) {
                last_status = 127;
                continue;
            }
            posish_errorf("wait: unsupported operand: %s", argv[i]);
            last_status = 1;
            continue;
        }

        interrupted_signal = 0;
        if (wait_for_pid(state, pid, &status, &interrupted_signal) != 0) {
            if (errno == ECHILD) {
                last_status = 127;
                continue;
            }
            if (errno == EINTR && interrupted_signal > 0) {
                return 128 + interrupted_signal;
            }
            perror("wait");
            return 1;
        }

        jobs_forget(pid);
        if (pid == state->last_async_pid) {
            state->last_async_pid = -1;
        }
        last_status = wait_status_to_shell_status(status);
    }

    return last_status;
}

static int builtin_fg(char *const argv[]) {
    pid_t pid;
    int status;

    if (argv[1] != NULL) {
        posish_errorf("fg: job specifiers are not supported yet");
        return 1;
    }

    pid = jobs_take_stopped();
    if (pid <= 0) {
        posish_errorf("fg: no current job");
        return 1;
    }

    /*
     * M1 shim: resume the latest stopped foreground process and wait for it.
     * Full job-table/process-group semantics come in later milestones.
     */
    if (kill(pid, SIGCONT) != 0) {
        perror("fg");
        return 1;
    }

    for (;;) {
        if (waitpid(pid, &status, WUNTRACED) < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("waitpid");
            return 1;
        }
        break;
    }

    if (WIFSTOPPED(status)) {
        jobs_note_stopped(pid);
    }
    return wait_status_to_shell_status(status);
}

static int builtin_make_command(char *const argv[]) {
    size_t i;

    for (i = 1; argv[i] != NULL; i++) {
        FILE *fp;

        fp = fopen(argv[i], "w");
        if (fp == NULL) {
            perror(argv[i]);
            return 1;
        }

        /* Helper utility used by imported POSIX tests to create tiny scripts. */
        fprintf(fp, "#!/bin/sh\n");
        fprintf(fp, "echo \"Running %s\"\n", argv[i]);
        if (fclose(fp) != 0) {
            perror(argv[i]);
            return 1;
        }

        if (chmod(argv[i], 0755) != 0) {
            perror(argv[i]);
            return 1;
        }
    }
    return 0;
}

int builtin_dispatch(struct shell_state *state, char *const argv[], bool *handled) {
    int status;

    status = builtin_try_special(state, argv, handled);
    if (*handled) {
        return status;
    }

    if (strcmp(argv[0], "cd") == 0) {
        *handled = true;
        return builtin_cd(argv);
    }
    if (strcmp(argv[0], "true") == 0) {
        *handled = true;
        return 0;
    }
    if (strcmp(argv[0], "false") == 0) {
        *handled = true;
        return 1;
    }
    if (strcmp(argv[0], "test") == 0 || strcmp(argv[0], "[") == 0) {
        *handled = true;
        return builtin_test(argv);
    }
    if (strcmp(argv[0], "kill") == 0) {
        *handled = true;
        return builtin_kill(argv);
    }
    if (strcmp(argv[0], "wait") == 0) {
        *handled = true;
        return builtin_wait(state, argv);
    }
    if (strcmp(argv[0], "fg") == 0) {
        *handled = true;
        return builtin_fg(argv);
    }
    if (strcmp(argv[0], "alias") == 0) {
        *handled = true;
        return builtin_alias(argv);
    }
    if (strcmp(argv[0], "unalias") == 0) {
        *handled = true;
        return builtin_unalias(argv);
    }
    if (strcmp(argv[0], "echoraw") == 0) {
        *handled = true;
        return builtin_echoraw(argv);
    }
    if (strcmp(argv[0], "bracket") == 0) {
        *handled = true;
        return builtin_bracket(argv);
    }
    if (strcmp(argv[0], "make_command") == 0) {
        *handled = true;
        return builtin_make_command(argv);
    }

    *handled = false;
    return 0;
}

bool builtin_is_name(const char *name) {
    static const char *const regular_names[] = {"cd",      "true",   "false",
                                                "test",    "[",      "kill",
                                                "wait",    "fg",     "alias",
                                                "unalias", "echoraw","bracket",
                                                "make_command"};
    size_t i;

    if (name == NULL || name[0] == '\0') {
        return false;
    }

    if (builtin_is_special_name(name)) {
        return true;
    }

    for (i = 0; i < sizeof(regular_names) / sizeof(regular_names[0]); i++) {
        if (strcmp(name, regular_names[i]) == 0) {
            return true;
        }
    }
    return false;
}
