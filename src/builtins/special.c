#include "builtins/builtin.h"

#include "error.h"
#include "exec.h"
#include "path.h"
#include "signals.h"
#include "trace.h"
#include "vars.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static bool inherited_ignore_locked(const struct shell_state *state, int signo) {
    return !state->interactive && signals_inherited_ignored(signo) &&
           !state->parent_was_interactive;
}

static void builtin_exec_prepare_signals(const struct shell_state *state) {
    /* Keep builtin `exec` behavior in lockstep with external command spawning. */
    exec_prepare_signals_for_exec_child(state);
}

static int builtin_exec(struct shell_state *state, char *const argv[]) {
    size_t i;
    int status;
    int saved_errno;

    i = 1;
    if (argv[i] != NULL && strcmp(argv[i], "--") == 0) {
        i++;
    }
    if (argv[i] == NULL) {
        return 0;
    }

    /*
     * `exec` replaces the shell with a utility process, so inherited trap
     * handlers are reset like any other external command spawn path.
     */
    trace_log(POSISH_TRACE_SIGNALS, "special builtin exec argv0=%s", argv[i]);
    (void)setenv("POSISH_PARENT_INTERACTIVE", state->interactive ? "1" : "0", 1);
    builtin_exec_prepare_signals(state);
    execvp(argv[i], &argv[i]);
    saved_errno = errno;
    status = saved_errno == ENOENT ? 127 : 126;
    perror(argv[i]);
    if (!state->interactive) {
        state->should_exit = true;
        state->exit_status = status;
    }
    return status;
}

static bool is_reserved_word_name(const char *name) {
    static const char *const words[] = {"!",   "{",    "}",    "case", "do",
                                        "done", "elif", "else", "esac", "fi",
                                        "for", "if",   "in",   "then", "until",
                                        "while"};
    size_t i;

    for (i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        if (strcmp(name, words[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool is_regular_builtin_name(const char *name) {
    static const char *const words[] = {"cd",      "true",      "false",
                                        "test",    "[",         "kill",
                                        "wait",    "fg",        "alias",
                                        "unalias", "echoraw",   "bracket",
                                        "make_command"};
    size_t i;

    for (i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        if (strcmp(name, words[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool has_shell_function(const struct shell_state *state,
                               const char *name) {
    size_t i;

    for (i = 0; i < state->function_count; i++) {
        if (strcmp(state->functions[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

static char *xstrdup_local(const char *s) {
    char *copy;

    copy = malloc(strlen(s) + 1);
    if (copy == NULL) {
        perror("malloc");
        return NULL;
    }
    strcpy(copy, s);
    return copy;
}

static char *find_command_path(const char *name, bool use_standard_path) {
    const char *path;
    const char *p;

    if (strchr(name, '/') != NULL) {
        if (access(name, X_OK) == 0) {
            char *cwd;
            char *absolute;
            size_t clen;
            size_t nlen;

            if (name[0] == '/') {
                return xstrdup_local(name);
            }
            cwd = path_getcwd_alloc();
            if (cwd == NULL) {
                return xstrdup_local(name);
            }
            clen = strlen(cwd);
            nlen = strlen(name);
            absolute = malloc(clen + 1 + nlen + 1);
            if (absolute == NULL) {
                perror("malloc");
                free(cwd);
                return NULL;
            }
            memcpy(absolute, cwd, clen);
            absolute[clen] = '/';
            memcpy(absolute + clen + 1, name, nlen + 1);
            free(cwd);
            return absolute;
        }
        return NULL;
    }

    path = use_standard_path ? "/bin:/usr/bin" : getenv("PATH");
    if (path == NULL || path[0] == '\0') {
        return NULL;
    }

    p = path;
    while (1) {
        const char *end;
        size_t dlen;
        const char *dir;
        char *candidate;

        end = strchr(p, ':');
        if (end == NULL) {
            end = p + strlen(p);
        }
        dlen = (size_t)(end - p);
        dir = dlen == 0 ? "." : p;

        candidate = malloc((dlen == 0 ? 1 : dlen) + 1 + strlen(name) + 1);
        if (candidate == NULL) {
            perror("malloc");
            return NULL;
        }
        if (dlen == 0) {
            strcpy(candidate, ".");
        } else {
            memcpy(candidate, dir, dlen);
            candidate[dlen] = '\0';
        }
        strcat(candidate, "/");
        strcat(candidate, name);

        if (access(candidate, X_OK) == 0) {
            return candidate;
        }
        free(candidate);

        if (*end == '\0') {
            break;
        }
        p = end + 1;
    }

    return NULL;
}

static char *find_dot_script_path(const char *name) {
    const char *path;
    const char *p;

    if (strchr(name, '/') != NULL) {
        if (access(name, R_OK) == 0) {
            return xstrdup_local(name);
        }
        return NULL;
    }

    path = getenv("PATH");
    if (path == NULL || path[0] == '\0') {
        path = ".";
    }
    p = path;

    while (1) {
        const char *colon;
        size_t dir_len;
        const char *dir;
        size_t name_len;
        char *candidate;

        colon = strchr(p, ':');
        dir_len = colon == NULL ? strlen(p) : (size_t)(colon - p);
        dir = p;
        if (dir_len == 0) {
            dir = ".";
            dir_len = 1;
        }

        name_len = strlen(name);
        candidate = malloc(dir_len + 1 + name_len + 1);
        if (candidate == NULL) {
            perror("malloc");
            return NULL;
        }
        memcpy(candidate, dir, dir_len);
        candidate[dir_len] = '/';
        memcpy(candidate + dir_len + 1, name, name_len + 1);

        if (access(candidate, R_OK) == 0) {
            return candidate;
        }
        free(candidate);

        if (colon == NULL) {
            break;
        }
        p = colon + 1;
    }

    return NULL;
}

static char *command_alias_value_dup(const char *name) {
    static const char prefix[] = "POSISH_ALIAS_";
    size_t plen;
    size_t nlen;
    char *key;
    const char *value;
    char *copy;

    plen = sizeof(prefix) - 1;
    nlen = strlen(name);
    key = malloc(plen + nlen + 1);
    if (key == NULL) {
        perror("malloc");
        return NULL;
    }
    memcpy(key, prefix, plen);
    memcpy(key + plen, name, nlen + 1);
    value = getenv(key);
    free(key);
    if (value == NULL) {
        return NULL;
    }
    copy = xstrdup_local(value);
    return copy;
}

static int builtin_command_describe(struct shell_state *state, char *const argv[],
                                    size_t start, bool use_standard_path,
                                    bool verbose) {
    size_t i;
    int status;

    status = 0;
    for (i = start; argv[i] != NULL; i++) {
        char *path;

        if (is_reserved_word_name(argv[i]) || builtin_is_special_name(argv[i])) {
            path = xstrdup_local(argv[i]);
        } else if (has_shell_function(state, argv[i])) {
            path = xstrdup_local(argv[i]);
        } else if ((path = command_alias_value_dup(argv[i])) != NULL) {
            if (verbose) {
                printf("%s is an alias for %s\n", argv[i], path);
            } else {
                /*
                 * Keep output eval-friendly for tests that round-trip
                 * `command -v alias_name` through `eval`.
                 */
                printf("%s() { %s; }\n", argv[i], path);
            }
            fflush(stdout);
            free(path);
            continue;
        } else {
            path = find_command_path(argv[i], use_standard_path);
            if (path == NULL && is_regular_builtin_name(argv[i])) {
                path = xstrdup_local(argv[i]);
            }
        }

        if (path == NULL) {
            status = 1;
            continue;
        }

        if (verbose) {
            printf("%s\n", path);
        } else {
            printf("%s\n", path);
        }
        fflush(stdout);
        free(path);
    }

    return status;
}

static int wait_status_to_shell_status(int wstatus) {
    if (WIFEXITED(wstatus)) {
        return WEXITSTATUS(wstatus);
    }
    if (WIFSIGNALED(wstatus)) {
        return 128 + WTERMSIG(wstatus);
    }
    if (WIFSTOPPED(wstatus)) {
        return 128 + WSTOPSIG(wstatus);
    }
    return 1;
}

static int default_status_for_flow_builtin(const struct shell_state *state) {
    if (state->main_context &&
        (state->running_signal_trap || state->running_exit_trap) &&
        state->trap_entry_status_valid) {
        return state->trap_entry_status;
    }
    return state->last_status;
}

static int builtin_command(struct shell_state *state, char *const argv[]) {
    size_t i;
    bool opt_v;
    bool opt_V;
    bool opt_p;
    bool handled;
    int status;
    bool restore_path;
    const char *saved_path;
    char *saved_path_copy;

    i = 1;
    opt_v = false;
    opt_V = false;
    opt_p = false;
    handled = false;
    status = 0;
    restore_path = false;
    saved_path = NULL;
    saved_path_copy = NULL;
    while (argv[i] != NULL) {
        size_t j;

        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (argv[i][0] != '-' || argv[i][1] == '\0') {
            break;
        }

        for (j = 1; argv[i][j] != '\0'; j++) {
            if (argv[i][j] == 'v') {
                opt_v = true;
                opt_V = false;
                continue;
            }
            if (argv[i][j] == 'V') {
                opt_V = true;
                opt_v = false;
                continue;
            }
            if (argv[i][j] == 'p') {
                opt_p = true;
                continue;
            }

            posish_errorf("command: invalid option: -%c", argv[i][j]);
            return 2;
        }
        i++;
    }

    if (opt_v || opt_V) {
        return builtin_command_describe(state, argv, i, opt_p, opt_V);
    }

    if (argv[i] == NULL) {
        return 0;
    }

    if (opt_p) {
        saved_path = getenv("PATH");
        if (saved_path != NULL) {
            saved_path_copy = xstrdup_local(saved_path);
        }
        if (setenv("PATH", "/bin:/usr/bin", 1) != 0) {
            perror("setenv");
            free(saved_path_copy);
            return 1;
        }
        restore_path = true;
    }

    {
        bool saved_in_command_builtin;

        saved_in_command_builtin = state->in_command_builtin;
        state->in_command_builtin = true;
        status = builtin_dispatch(state, &argv[i], &handled);
        state->in_command_builtin = saved_in_command_builtin;
    }
    if (!handled) {
        pid_t pid;
        int wstatus;

        pid = fork();
        if (pid < 0) {
            perror("fork");
            status = 1;
            goto done;
        }

        if (pid == 0) {
            int saved_errno;

            execvp(argv[i], &argv[i]);
            saved_errno = errno;
            perror(argv[i]);
            _exit(saved_errno == ENOENT ? 127 : 126);
        }

        for (;;) {
            if (waitpid(pid, &wstatus, 0) < 0) {
                if (errno == EINTR) {
                    continue;
                }
                perror("waitpid");
                status = 1;
                goto done;
            }
            break;
        }
        status = wait_status_to_shell_status(wstatus);
    }

done:
    if (restore_path) {
        if (saved_path_copy != NULL) {
            if (setenv("PATH", saved_path_copy, 1) != 0) {
                perror("setenv");
            }
        } else {
            if (unsetenv("PATH") != 0) {
                perror("unsetenv");
            }
        }
    }
    free(saved_path_copy);
    return status;
}

static int builtin_set(struct shell_state *state, char *const argv[]) {
    size_t i;
    bool refresh_signal_policy;

    i = 1;
    refresh_signal_policy = false;
    while (argv[i] != NULL) {
        const char *opt;
        size_t j;

        opt = argv[i];
        if (strcmp(opt, "--") == 0) {
            return 0;
        }
        if ((opt[0] != '-' && opt[0] != '+') || opt[1] == '\0') {
            return 0;
        }
        for (j = 1; opt[j] != '\0'; j++) {
            if (!isalpha((unsigned char)opt[j])) {
                posish_errorf("set: invalid option: %s", argv[i]);
                return 2;
            }
            if (opt[j] == 'e') {
                state->errexit = opt[0] == '-';
            } else if (opt[j] == 'i') {
                bool new_interactive;

                new_interactive = opt[0] == '-';
                if (state->interactive != new_interactive) {
                    refresh_signal_policy = true;
                }
                state->interactive = new_interactive;
            } else if (opt[j] == 'm') {
                bool new_monitor_mode;

                new_monitor_mode = opt[0] == '-';
                if (state->monitor_mode != new_monitor_mode) {
                    refresh_signal_policy = true;
                }
                state->monitor_mode = new_monitor_mode;
            } else if (opt[j] == 'v') {
                state->verbose = opt[0] == '-';
            }
        }
        i++;
    }

    if (refresh_signal_policy) {
        shell_refresh_signal_policy(state);
    }
    return 0;
}

static int parse_loop_count_operand(const char *name, const char *text,
                                    int *count_out) {
    long n;
    char *end;

    errno = 0;
    n = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || n <= 0 || n > INT_MAX) {
        posish_errorf("%s: invalid loop count: %s", name, text);
        return 1;
    }

    *count_out = (int)n;
    return 0;
}

static int builtin_break_or_continue(struct shell_state *state, char *const argv[],
                                     bool is_continue) {
    const char *name;
    int count;

    name = is_continue ? "continue" : "break";
    count = 1;

    if (argv[1] != NULL) {
        if (parse_loop_count_operand(name, argv[1], &count) != 0) {
            return 1;
        }
        if (argv[2] != NULL) {
            posish_errorf("%s: too many arguments", name);
            return 1;
        }
    }

    if (state->loop_depth <= 0) {
        posish_errorf("%s: only meaningful in a loop", name);
        return 1;
    }

    if (is_continue) {
        state->continue_levels = count;
        state->break_levels = 0;
    } else {
        state->break_levels = count;
        state->continue_levels = 0;
    }
    return 0;
}

static int builtin_return(struct shell_state *state, char *const argv[]) {
    size_t i;
    int status;
    long n;
    char *end;

    i = 1;
    if (argv[i] != NULL && strcmp(argv[i], "--") == 0) {
        i++;
    }

    status = default_status_for_flow_builtin(state);
    if (argv[i] != NULL) {
        errno = 0;
        n = strtol(argv[i], &end, 10);
        if (errno != 0 || end == argv[i] || *end != '\0') {
            posish_errorf("return: numeric argument required: %s", argv[i]);
            return 2;
        }
        status = (unsigned char)n;
        i++;
    }

    if (argv[i] != NULL) {
        posish_errorf("return: too many arguments");
        return 1;
    }

    if (state->function_depth <= 0 && state->dot_depth <= 0) {
        posish_errorf("return: can only be used in a function or sourced script");
        return 1;
    }

    state->return_requested = true;
    state->return_status = status;
    return status;
}

static int builtin_dot(struct shell_state *state, char *const argv[]) {
    size_t i;
    char *path;
    int status;
    bool saved_interactive;

    i = 1;
    if (argv[i] != NULL && strcmp(argv[i], "--") == 0) {
        i++;
    }
    if (argv[i] == NULL) {
        posish_errorf(".: filename argument required");
        return 2;
    }

    path = find_dot_script_path(argv[i]);
    if (path == NULL) {
        posish_errorf(".: %s: file not found", argv[i]);
        if (!state->interactive && !state->in_command_builtin) {
            state->should_exit = true;
            state->exit_status = 1;
        }
        return 1;
    }

    /*
     * Source execution runs in the current shell but should not permanently
     * flip interactive mode just because shell_run_file uses non-interactive
     * stream parsing internally.
     */
    saved_interactive = state->interactive;
    state->dot_depth++;
    status = shell_run_file(state, path);
    state->dot_depth--;
    state->interactive = saved_interactive;
    free(path);

    if (state->return_requested) {
        status = state->return_status;
        state->return_requested = false;
    }
    return status;
}

static int split_assignment(const char *word, char **name_out, const char **value_out) {
    const char *eq;
    size_t nlen;
    char *name;

    eq = strchr(word, '=');
    if (eq == NULL || eq == word) {
        return -1;
    }

    nlen = (size_t)(eq - word);
    name = malloc(nlen + 1);
    if (name == NULL) {
        perror("malloc");
        return -1;
    }
    memcpy(name, word, nlen);
    name[nlen] = '\0';

    *name_out = name;
    *value_out = eq + 1;
    return 0;
}

static bool command_is_blank(const char *command) {
    size_t i;

    for (i = 0; command[i] != '\0'; i++) {
        if (!isspace((unsigned char)command[i])) {
            return false;
        }
    }
    return true;
}

static char *double_quote_for_eval(const char *value) {
    size_t i;
    size_t len;
    char *out;
    size_t pos;

    len = 2;
    for (i = 0; value[i] != '\0'; i++) {
        if (value[i] == '\\' || value[i] == '"' || value[i] == '$' ||
            value[i] == '`') {
            len += 2;
        } else {
            len += 1;
        }
    }

    out = malloc(len + 1);
    if (out == NULL) {
        perror("malloc");
        return NULL;
    }

    pos = 0;
    out[pos++] = '"';
    for (i = 0; value[i] != '\0'; i++) {
        if (value[i] == '\\' || value[i] == '"' || value[i] == '$' ||
            value[i] == '`') {
            out[pos++] = '\\';
            out[pos++] = value[i];
        } else {
            out[pos++] = value[i];
        }
    }
    out[pos++] = '"';
    out[pos] = '\0';
    return out;
}

static int print_exported_variables(void) {
    size_t i;

    for (i = 0; environ != NULL && environ[i] != NULL; i++) {
        const char *entry;
        const char *eq;
        size_t nlen;
        char *name;
        char *quoted;

        entry = environ[i];
        eq = strchr(entry, '=');
        if (eq == NULL) {
            continue;
        }

        nlen = (size_t)(eq - entry);
        name = malloc(nlen + 1);
        if (name == NULL) {
            perror("malloc");
            return 1;
        }
        memcpy(name, entry, nlen);
        name[nlen] = '\0';
        if (!vars_is_name_valid(name)) {
            free(name);
            continue;
        }

        quoted = double_quote_for_eval(eq + 1);
        if (quoted == NULL) {
            free(name);
            return 1;
        }
        printf("export %s=%s\n", name, quoted);
        free(quoted);
        free(name);
    }
    fflush(stdout);

    return 0;
}

static int print_readonly_variables(const struct shell_state *state) {
    size_t i;

    for (i = 0; i < state->readonly_count; i++) {
        const char *name;
        const char *value;
        char *quoted;

        name = state->readonly_names[i];
        value = getenv(name);
        if (value == NULL) {
            value = "";
        }

        quoted = double_quote_for_eval(value);
        if (quoted == NULL) {
            return 1;
        }
        printf("readonly %s=%s\n", name, quoted);
        free(quoted);
    }
    fflush(stdout);

    return 0;
}

static int builtin_unset(struct shell_state *state, char *const argv[]) {
    size_t i;
    int status;

    status = 0;
    for (i = 1; argv[i] != NULL; i++) {
        if (vars_unset(state, argv[i]) != 0) {
            status = 1;
        }
    }
    return status;
}

static int builtin_export(struct shell_state *state, char *const argv[]) {
    size_t i;
    int status;
    bool print_only;

    i = 1;
    print_only = false;
    while (argv[i] != NULL) {
        size_t j;

        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (argv[i][0] != '-' || argv[i][1] == '\0') {
            break;
        }

        for (j = 1; argv[i][j] != '\0'; j++) {
            if (argv[i][j] == 'p') {
                print_only = true;
            } else {
                posish_errorf("export: invalid option: -%c", argv[i][j]);
                return 2;
            }
        }
        i++;
    }

    if (argv[i] == NULL) {
        /*
         * POSIX allows `export` with no operands to print export declarations.
         */
        return print_exported_variables();
    }

    status = 0;
    for (; argv[i] != NULL; i++) {
        char *name;
        const char *value;

        if (strcmp(argv[i], "--") == 0) {
            continue;
        }
        if (split_assignment(argv[i], &name, &value) == 0) {
            if (vars_set(state, name, value, true) != 0) {
                status = 1;
            }
            free(name);
            continue;
        }

        if (!vars_is_name_valid(argv[i])) {
            posish_errorf("export: invalid variable name: %s", argv[i]);
            status = 1;
            continue;
        }

        if (getenv(argv[i]) == NULL) {
            if (setenv(argv[i], "", 1) != 0) {
                perror("setenv");
                status = 1;
            }
        }
    }

    if (status == 0 && print_only) {
        status = print_exported_variables();
    }
    if (status != 0 && !state->interactive) {
        state->should_exit = true;
        state->exit_status = status;
    }
    return status;
}

static int builtin_eval(struct shell_state *state, char *const argv[]) {
    size_t i;
    size_t len;
    char *command;
    size_t pos;
    int status;

    i = 1;
    if (argv[i] != NULL && strcmp(argv[i], "--") == 0) {
        i++;
    }

    if (argv[i] == NULL) {
        return 0;
    }

    len = 1;
    for (; argv[i] != NULL; i++) {
        len += strlen(argv[i]) + 1;
    }

    command = malloc(len);
    if (command == NULL) {
        perror("malloc");
        return 1;
    }

    pos = 0;
    for (i = (argv[1] != NULL && strcmp(argv[1], "--") == 0) ? 2 : 1;
         argv[i] != NULL; i++) {
        size_t n;

        if (pos > 0) {
            command[pos++] = ' ';
        }
        n = strlen(argv[i]);
        memcpy(command + pos, argv[i], n);
        pos += n;
    }
    command[pos] = '\0';

    if (command_is_blank(command)) {
        free(command);
        return 0;
    }

    status = shell_run_command(state, command);
    free(command);
    return status;
}

static int builtin_read(struct shell_state *state, char *const argv[]) {
    char *line;
    size_t cap;
    ssize_t nread;
    int status;

    line = NULL;
    cap = 0;
    clearerr(stdin);
    nread = getline(&line, &cap, stdin);
    if (nread < 0) {
        free(line);
        return 1;
    }
    if (nread > 0 && line[nread - 1] == '\n') {
        line[nread - 1] = '\0';
    }

    if (argv[1] == NULL) {
        free(line);
        return 0;
    }

    status = vars_set(state, argv[1], line, true);
    free(line);
    return status;
}

static int builtin_readonly(struct shell_state *state, char *const argv[]) {
    size_t i;
    int status;
    bool print_only;

    i = 1;
    print_only = false;
    while (argv[i] != NULL) {
        size_t j;

        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (argv[i][0] != '-' || argv[i][1] == '\0') {
            break;
        }

        for (j = 1; argv[i][j] != '\0'; j++) {
            if (argv[i][j] == 'p') {
                print_only = true;
            } else {
                posish_errorf("readonly: invalid option: -%c", argv[i][j]);
                return 2;
            }
        }
        i++;
    }

    if (argv[i] == NULL) {
        return print_readonly_variables(state);
    }

    status = 0;
    for (; argv[i] != NULL; i++) {
        char *name;
        const char *value;

        if (strcmp(argv[i], "--") == 0) {
            continue;
        }
        if (split_assignment(argv[i], &name, &value) == 0) {
            if (vars_mark_readonly(state, name, value, true) != 0) {
                status = 1;
            }
            free(name);
            continue;
        }

        if (vars_mark_readonly(state, argv[i], "", false) != 0) {
            status = 1;
        }
    }

    if (status == 0 && print_only) {
        status = print_readonly_variables(state);
    }
    if (status != 0 && !state->interactive) {
        state->should_exit = true;
        state->exit_status = status;
    }
    return status;
}

static int trap_signal_number(const char *spec) {
    const char *name;
    char *end;
    long n;

    if (spec == NULL || spec[0] == '\0') {
        return -1;
    }

    n = strtol(spec, &end, 10);
    if (*end == '\0' && n > 0 && n <= NSIG) {
        return (int)n;
    }

    name = spec;
    if (strncmp(name, "SIG", 3) == 0) {
        name += 3;
    }

#ifdef SIGHUP
    if (strcmp(name, "HUP") == 0) return SIGHUP;
#endif
#ifdef SIGINT
    if (strcmp(name, "INT") == 0) return SIGINT;
#endif
#ifdef SIGQUIT
    if (strcmp(name, "QUIT") == 0) return SIGQUIT;
#endif
#ifdef SIGTERM
    if (strcmp(name, "TERM") == 0) return SIGTERM;
#endif
#ifdef SIGCHLD
    if (strcmp(name, "CHLD") == 0) return SIGCHLD;
    if (strcmp(name, "CLD") == 0) return SIGCHLD;
#endif
#ifdef SIGPIPE
    if (strcmp(name, "PIPE") == 0) return SIGPIPE;
#endif
#ifdef SIGUSR1
    if (strcmp(name, "USR1") == 0) return SIGUSR1;
#endif
#ifdef SIGUSR2
    if (strcmp(name, "USR2") == 0) return SIGUSR2;
#endif
#ifdef SIGALRM
    if (strcmp(name, "ALRM") == 0) return SIGALRM;
#endif
#ifdef SIGABRT
    if (strcmp(name, "ABRT") == 0) return SIGABRT;
    if (strcmp(name, "IOT") == 0) return SIGABRT;
#endif
#ifdef SIGBUS
    if (strcmp(name, "BUS") == 0) return SIGBUS;
#endif
#ifdef SIGFPE
    if (strcmp(name, "FPE") == 0) return SIGFPE;
#endif
#ifdef SIGILL
    if (strcmp(name, "ILL") == 0) return SIGILL;
#endif
#ifdef SIGSEGV
    if (strcmp(name, "SEGV") == 0) return SIGSEGV;
#endif
#ifdef SIGTRAP
    if (strcmp(name, "TRAP") == 0) return SIGTRAP;
#endif
#ifdef SIGPOLL
    if (strcmp(name, "POLL") == 0) return SIGPOLL;
#endif
#ifdef SIGPROF
    if (strcmp(name, "PROF") == 0) return SIGPROF;
#endif
#ifdef SIGSYS
    if (strcmp(name, "SYS") == 0) return SIGSYS;
#endif
#ifdef SIGVTALRM
    if (strcmp(name, "VTALRM") == 0) return SIGVTALRM;
#endif
#ifdef SIGXCPU
    if (strcmp(name, "XCPU") == 0) return SIGXCPU;
#endif
#ifdef SIGXFSZ
    if (strcmp(name, "XFSZ") == 0) return SIGXFSZ;
#endif
#ifdef SIGCONT
    if (strcmp(name, "CONT") == 0) return SIGCONT;
#endif
#ifdef SIGSTOP
    if (strcmp(name, "STOP") == 0) return SIGSTOP;
#endif
#ifdef SIGTSTP
    if (strcmp(name, "TSTP") == 0) return SIGTSTP;
#endif
#ifdef SIGTTIN
    if (strcmp(name, "TTIN") == 0) return SIGTTIN;
#endif
#ifdef SIGTTOU
    if (strcmp(name, "TTOU") == 0) return SIGTTOU;
#endif
#ifdef SIGURG
    if (strcmp(name, "URG") == 0) return SIGURG;
#endif
    return -1;
}

static int trap_set_signal_action(int signo, void (*handler)(int)) {
    if (handler == SIG_IGN) {
        return signals_set_ignored(signo);
    }
    if (handler == SIG_DFL) {
        return signals_set_default(signo);
    }
    return signals_set_trap(signo);
}

static int trap_set_signal_trap(int signo) {
    return signals_set_trap(signo);
}

static int trap_set_signal_command(struct shell_state *state, int signo,
                                   const char *command) {
    char *copy;

    copy = malloc(strlen(command) + 1);
    if (copy == NULL) {
        perror("malloc");
        return -1;
    }
    strcpy(copy, command);

    free(state->signal_traps[signo]);
    state->signal_traps[signo] = copy;
    signals_clear_pending(signo);
    return 0;
}

static void trap_restore_default_or_inherited_ignore(const struct shell_state *state,
                                                     int signo) {
    int rc;

    /*
     * `trap -` restores the action at shell invocation:
     * - truly inherited SIG_IGN always stays ignored
     * - interactive startup policy ignores apply only in the main shell
     */
    if (state->interactive && state->main_context &&
        signals_policy_ignored(signo)) {
        rc = trap_set_signal_action(signo, SIG_IGN);
    } else if (inherited_ignore_locked(state, signo)) {
        rc = trap_set_signal_action(signo, SIG_IGN);
    } else {
        rc = trap_set_signal_action(signo, SIG_DFL);
    }
    if (rc != 0) {
        perror("trap");
    }
}

static int builtin_trap(struct shell_state *state, char *const argv[]) {
    size_t i;
    int status;
    const char *action;

    if (argv[1] == NULL) {
        return 0;
    }

    if (argv[2] == NULL) {
        posish_errorf("trap: missing condition");
        return 2;
    }

    action = argv[1];
    trace_log(POSISH_TRACE_TRAPS, "trap action='%s'", action);
    status = 0;
    for (i = 2; argv[i] != NULL; i++) {
        if (strcmp(argv[i], "EXIT") == 0 || strcmp(argv[i], "0") == 0) {
            if (strcmp(action, "-") == 0) {
                free(state->exit_trap);
                state->exit_trap = NULL;
                trace_log(POSISH_TRACE_TRAPS, "trap EXIT cleared");
            } else {
                char *command;

                command = malloc(strlen(action) + 1);
                if (command == NULL) {
                    perror("malloc");
                    return 1;
                }
                strcpy(command, action);

                free(state->exit_trap);
                state->exit_trap = command;
                trace_log(POSISH_TRACE_TRAPS, "trap EXIT set command=%s",
                          state->exit_trap);
            }
            continue;
        } else {
            int signo;

            signo = trap_signal_number(argv[i]);
            if (signo < 0) {
                posish_errorf("trap: invalid signal: %s", argv[i]);
                status = 1;
                continue;
            }

            if (strcmp(action, "-") == 0) {
                free(state->signal_traps[signo]);
                state->signal_traps[signo] = NULL;
                state->signal_cleared[signo] = !inherited_ignore_locked(state, signo);
                signals_clear_pending(signo);
                trap_restore_default_or_inherited_ignore(state, signo);
                trace_log(POSISH_TRACE_TRAPS,
                          "trap clear signo=%d cleared=%d inherited_lock=%d",
                          signo, state->signal_cleared[signo] ? 1 : 0,
                          inherited_ignore_locked(state, signo) ? 1 : 0);
            } else if (action[0] == '\0') {
                state->signal_cleared[signo] = false;
                if (trap_set_signal_command(state, signo, "") != 0) {
                    status = 1;
                    continue;
                }
                if (trap_set_signal_action(signo, SIG_IGN) != 0) {
                    perror("trap");
                    status = 1;
                }
                trace_log(POSISH_TRACE_TRAPS, "trap ignore signo=%d", signo);
            } else {
                /*
                 * In non-interactive shells, inherited SIG_IGN remains sticky.
                 */
                if (inherited_ignore_locked(state, signo)) {
                    free(state->signal_traps[signo]);
                    state->signal_traps[signo] = NULL;
                    state->signal_cleared[signo] = false;
                    signals_clear_pending(signo);
                    if (trap_set_signal_action(signo, SIG_IGN) != 0) {
                        perror("trap");
                        status = 1;
                    }
                    trace_log(POSISH_TRACE_TRAPS,
                              "trap command ignored due inherited lock signo=%d",
                              signo);
                    continue;
                }

                state->signal_cleared[signo] = false;
                if (trap_set_signal_command(state, signo, action) != 0) {
                    status = 1;
                    continue;
                }
                if (trap_set_signal_trap(signo) != 0) {
                    perror("trap");
                    status = 1;
                }
                trace_log(POSISH_TRACE_TRAPS, "trap command set signo=%d cmd=%s",
                          signo, action);
            }
        }
    }

    return status;
}

int builtin_try_special(struct shell_state *state, char *const argv[], bool *handled) {
    if (strcmp(argv[0], ".") == 0) {
        *handled = true;
        return builtin_dot(state, argv);
    }

    if (strcmp(argv[0], ":") == 0) {
        *handled = true;
        return 0;
    }

    if (strcmp(argv[0], "exec") == 0) {
        *handled = true;
        return builtin_exec(state, argv);
    }

    if (strcmp(argv[0], "break") == 0) {
        *handled = true;
        return builtin_break_or_continue(state, argv, false);
    }

    if (strcmp(argv[0], "continue") == 0) {
        *handled = true;
        return builtin_break_or_continue(state, argv, true);
    }

    if (strcmp(argv[0], "return") == 0) {
        *handled = true;
        return builtin_return(state, argv);
    }

    if (strcmp(argv[0], "set") == 0) {
        *handled = true;
        return builtin_set(state, argv);
    }

    if (strcmp(argv[0], "unset") == 0) {
        *handled = true;
        return builtin_unset(state, argv);
    }

    if (strcmp(argv[0], "export") == 0) {
        *handled = true;
        return builtin_export(state, argv);
    }

    if (strcmp(argv[0], "eval") == 0) {
        *handled = true;
        return builtin_eval(state, argv);
    }

    if (strcmp(argv[0], "read") == 0) {
        *handled = true;
        return builtin_read(state, argv);
    }

    if (strcmp(argv[0], "readonly") == 0) {
        *handled = true;
        return builtin_readonly(state, argv);
    }

    if (strcmp(argv[0], "trap") == 0) {
        *handled = true;
        return builtin_trap(state, argv);
    }

    if (strcmp(argv[0], "command") == 0) {
        *handled = true;
        return builtin_command(state, argv);
    }

    if (strcmp(argv[0], "exit") == 0) {
        size_t i;
        int status;
        char *end;

        i = 1;
        if (argv[i] != NULL && strcmp(argv[i], "--") == 0) {
            i++;
        }

        status = default_status_for_flow_builtin(state);
        if (argv[i] != NULL) {
            errno = 0;
            status = (int)strtol(argv[i], &end, 10);
            if (errno != 0 || end == argv[i] || *end != '\0') {
                posish_errorf("exit: numeric argument required: %s", argv[i]);
                status = 2;
            }
            i++;
            if (argv[i] != NULL) {
                posish_errorf("exit: too many arguments");
                *handled = true;
                return 1;
            }
        }

        state->should_exit = true;
        state->exit_status = status;
        *handled = true;
        return status;
    }

    *handled = false;
    return 0;
}

bool builtin_is_special_name(const char *name) {
    static const char *const names[] = {".",     ":",      "break", "continue",
                                        "eval",  "exec",   "exit",  "export",
                                        "readonly", "return", "set",   "shift",
                                        "command",
                                        "times", "trap",   "unset"};
    size_t i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcmp(name, names[i]) == 0) {
            return true;
        }
    }
    return false;
}
