/* SPDX-License-Identifier: 0BSD */

/* posish - regular builtins */

#include "builtins/builtin.h"
#include "builtins/netbsd_test.h"

#include "error.h"
#include "jobs.h"
#include "path.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
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

static int parse_octal_umask(const char *text, mode_t *mask_out) {
    char *end;
    unsigned long value;

    if (text == NULL || text[0] == '\0') {
        return -1;
    }

    errno = 0;
    value = strtoul(text, &end, 8);
    if (errno != 0 || *end != '\0' || value > 0777UL) {
        return -1;
    }

    *mask_out = (mode_t)value;
    return 0;
}

static mode_t who_mask_for_class(char cls) {
    if (cls == 'u') {
        return 0700;
    }
    if (cls == 'g') {
        return 0070;
    }
    return 0007;
}

static unsigned mode_triplet(mode_t mode, char cls) {
    if (cls == 'u') {
        return (unsigned)((mode >> 6) & 07);
    }
    if (cls == 'g') {
        return (unsigned)((mode >> 3) & 07);
    }
    return (unsigned)(mode & 07);
}

static mode_t copy_triplet_to_targets(unsigned triplet, mode_t who) {
    mode_t bits;

    bits = 0;
    if ((who & 0700) != 0) {
        bits |= (mode_t)(triplet << 6);
    }
    if ((who & 0070) != 0) {
        bits |= (mode_t)(triplet << 3);
    }
    if ((who & 0007) != 0) {
        bits |= (mode_t)triplet;
    }
    return bits;
}

static mode_t perm_bits_for_targets(mode_t who, char perm) {
    mode_t bits;

    bits = 0;
    if (perm == 'r') {
        if ((who & 0700) != 0) {
            bits |= 0400;
        }
        if ((who & 0070) != 0) {
            bits |= 0040;
        }
        if ((who & 0007) != 0) {
            bits |= 0004;
        }
        return bits;
    }
    if (perm == 'w') {
        if ((who & 0700) != 0) {
            bits |= 0200;
        }
        if ((who & 0070) != 0) {
            bits |= 0020;
        }
        if ((who & 0007) != 0) {
            bits |= 0002;
        }
        return bits;
    }
    if ((who & 0700) != 0) {
        bits |= 0100;
    }
    if ((who & 0070) != 0) {
        bits |= 0010;
    }
    if ((who & 0007) != 0) {
        bits |= 0001;
    }
    return bits;
}

static int parse_symbolic_umask(const char *spec, mode_t *mask_out) {
    const char *p;
    mode_t mode;

    if (spec == NULL || spec[0] == '\0') {
        return -1;
    }

    p = spec;
    mode = (mode_t)(~(*mask_out) & 0777);
    while (*p != '\0') {
        mode_t who;
        bool have_who;

        who = 0;
        have_who = false;
        while (*p == 'u' || *p == 'g' || *p == 'o' || *p == 'a') {
            have_who = true;
            if (*p == 'a') {
                who |= 0777;
            } else {
                who |= who_mask_for_class(*p);
            }
            p++;
        }
        if (!have_who) {
            who = 0777;
        }

        if (*p != '+' && *p != '-' && *p != '=') {
            return -1;
        }

        while (*p == '+' || *p == '-' || *p == '=') {
            char op;
            mode_t bits;

            op = *p++;
            bits = 0;
            while (*p != '\0' && *p != ',' && *p != '+' && *p != '-' &&
                   *p != '=') {
                char ch;

                ch = *p++;
                if (ch == 'r' || ch == 'w' || ch == 'x') {
                    bits |= perm_bits_for_targets(who, ch);
                } else if (ch == 'u' || ch == 'g' || ch == 'o') {
                    bits |= copy_triplet_to_targets(mode_triplet(mode, ch), who);
                } else {
                    return -1;
                }
            }

            if (op == '=') {
                mode &= (mode_t)~who;
                mode |= (bits & who);
            } else if (op == '+') {
                mode |= bits;
            } else {
                mode &= (mode_t)~bits;
            }
        }

        if (*p == ',') {
            p++;
            if (*p == '\0') {
                return -1;
            }
            continue;
        }
        if (*p != '\0') {
            return -1;
        }
    }

    *mask_out = (mode_t)(~mode & 0777);
    return 0;
}

static void append_symbolic_triplet(char *buf, size_t *offset, unsigned triplet) {
    if ((triplet & 04) != 0) {
        buf[(*offset)++] = 'r';
    }
    if ((triplet & 02) != 0) {
        buf[(*offset)++] = 'w';
    }
    if ((triplet & 01) != 0) {
        buf[(*offset)++] = 'x';
    }
}

static void print_umask_symbolic(mode_t mask) {
    mode_t mode;
    char out[64];
    size_t off;

    mode = (mode_t)(~mask & 0777);
    off = 0;
    out[off++] = 'u';
    out[off++] = '=';
    append_symbolic_triplet(out, &off, mode_triplet(mode, 'u'));
    out[off++] = ',';
    out[off++] = 'g';
    out[off++] = '=';
    append_symbolic_triplet(out, &off, mode_triplet(mode, 'g'));
    out[off++] = ',';
    out[off++] = 'o';
    out[off++] = '=';
    append_symbolic_triplet(out, &off, mode_triplet(mode, 'o'));
    out[off] = '\0';
    puts(out);
}

static int builtin_umask(char *const argv[]) {
    size_t i;
    bool symbolic;
    const char *operand;
    mode_t current_mask;
    mode_t new_mask;

    i = 1;
    symbolic = false;
    while (argv[i] != NULL) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (strcmp(argv[i], "-S") == 0) {
            symbolic = true;
            i++;
            continue;
        }
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            posish_errorf("umask: invalid option: %s", argv[i]);
            return 2;
        }
        break;
    }

    operand = argv[i];
    if (operand != NULL && argv[i + 1] != NULL) {
        posish_errorf("umask: too many operands");
        return 1;
    }

    current_mask = umask(0);
    umask(current_mask);

    if (operand == NULL) {
        if (symbolic) {
            print_umask_symbolic(current_mask);
        } else {
            printf("%03o\n", (unsigned)(current_mask & 0777));
        }
        return 0;
    }

    new_mask = current_mask;
    if (parse_octal_umask(operand, &new_mask) != 0 &&
        parse_symbolic_umask(operand, &new_mask) != 0) {
        posish_errorf("umask: invalid mode: %s", operand);
        return 1;
    }

    umask(new_mask);
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
    size_t argc;

    argc = 0;
    while (argv[argc] != NULL) {
        argc++;
    }
    return posish_netbsd_test_builtin((int)argc, argv);
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

static bool is_decimal_number(const char *text) {
    size_t i;

    if (text == NULL || text[0] == '\0') {
        return false;
    }
    i = 0;
    if (text[i] == '-') {
        i++;
    }
    if (text[i] == '\0') {
        return false;
    }
    for (; text[i] != '\0'; i++) {
        if (!isdigit((unsigned char)text[i])) {
            return false;
        }
    }
    return true;
}

static size_t kill_operand_start(char *const argv[]) {
    size_t i;

    i = 1;
    while (argv[i] != NULL) {
        if (strcmp(argv[i], "--") == 0) {
            return i + 1;
        }
        if (argv[i][0] != '-' || argv[i][1] == '\0') {
            return i;
        }
        if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "-n") == 0) {
            if (argv[i + 1] == NULL) {
                return i + 1;
            }
            i += 2;
            continue;
        }
        if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--list") == 0 ||
            strncmp(argv[i], "--list=", 7) == 0) {
            return i + 1;
        }
        i++;
    }
    return i;
}

static int builtin_kill(char *const argv[]) {
    size_t argc;
    size_t i;
    char **converted;
    char **final_argv;
    size_t final_argc;
    size_t operand_start;
    bool insert_double_dash;
    bool had_jobspec;
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
    final_argv = NULL;
    final_argc = argc;
    operand_start = kill_operand_start(argv);
    insert_double_dash = false;
    had_jobspec = false;

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
            struct jobs_entry_info job;
            char *pid_text;

            if (!jobs_get_by_spec(argv[i], &job) || job.pgid <= 0) {
                posish_errorf("kill: no such job: %s", argv[i]);
                status = 1;
                goto done;
            }

            pid_text = pid_to_string(-job.pgid);
            if (pid_text == NULL) {
                perror("malloc");
                status = 1;
                goto done;
            }
            converted[i] = pid_text;
            had_jobspec = true;
        } else {
            converted[i] = argv[i];
        }
    }
    converted[argc] = NULL;

    if (!list_mode && had_jobspec && operand_start < argc &&
        converted[operand_start] != NULL &&
        is_decimal_number(converted[operand_start]) &&
        converted[operand_start][0] == '-') {
        insert_double_dash = true;
    }

    if (insert_double_dash) {
        size_t j;

        final_argv = calloc(argc + 2, sizeof(*final_argv));
        if (final_argv == NULL) {
            perror("calloc");
            status = 1;
            goto done;
        }
        for (j = 0; j < operand_start; j++) {
            final_argv[j] = converted[j];
        }
        final_argv[operand_start] = "--";
        for (j = operand_start; j < argc; j++) {
            final_argv[j + 1] = converted[j];
        }
        final_argv[argc + 1] = NULL;
        final_argc = argc + 1;
    }

    status = run_utility(final_argv != NULL ? final_argv : converted);

done:
    for (i = 0; i < argc; i++) {
        if (converted[i] != NULL && converted[i] != argv[i]) {
            free(converted[i]);
        }
    }
    if (final_argv != NULL) {
        for (i = 0; i < final_argc; i++) {
            final_argv[i] = NULL;
        }
        free(final_argv);
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

static int wait_foreground_job(struct shell_state *state,
                               const struct jobs_entry_info *job) {
    int status;
    int tty_fd;
    bool transferred_tty;
    bool close_tty;
    pid_t shell_pgid;
    pid_t job_pgid;

    transferred_tty = false;
    close_tty = false;
    shell_pgid = getpgrp();
    job_pgid = job->pgid > 0 ? job->pgid : job->pid;
    tty_fd = -1;
    if (state->monitor_mode) {
        if (isatty(STDIN_FILENO)) {
            tty_fd = STDIN_FILENO;
        } else {
            /*
             * Job control operates on the controlling terminal, which can
             * differ from stdin in harnessed test runs.
             */
            tty_fd = open("/dev/tty", O_RDWR);
            if (tty_fd >= 0) {
                close_tty = true;
            }
        }
    }

    if (tty_fd >= 0) {
        if (tcsetpgrp(tty_fd, job_pgid) == 0) {
            transferred_tty = true;
        }
    }

    jobs_mark_running(job->pid);
    if (kill(-job_pgid, SIGCONT) != 0) {
        if (transferred_tty) {
            (void)tcsetpgrp(tty_fd, shell_pgid);
        }
        if (close_tty) {
            close(tty_fd);
        }
        perror("fg");
        return 1;
    }

    for (;;) {
        if (waitpid(job->pid, &status, WUNTRACED) < 0) {
            if (errno == EINTR) {
                shell_run_pending_traps(state);
                continue;
            }
            if (transferred_tty) {
                (void)tcsetpgrp(tty_fd, shell_pgid);
            }
            if (close_tty) {
                close(tty_fd);
            }
            perror("waitpid");
            return 1;
        }
        break;
    }

    if (transferred_tty) {
        (void)tcsetpgrp(tty_fd, shell_pgid);
    }
    if (close_tty) {
        close(tty_fd);
    }

    if (WIFSTOPPED(status)) {
        jobs_note_stopped_with_command(job->pid, job_pgid, job->command);
    } else {
        jobs_forget(job->pid);
    }
    return wait_status_to_shell_status(status);
}

static int builtin_fg(struct shell_state *state, char *const argv[]) {
    struct jobs_entry_info job;

    if (!state->monitor_mode) {
        posish_errorf("fg: job control is disabled");
        return 1;
    }

    if (argv[1] != NULL) {
        if (argv[2] != NULL) {
            posish_errorf("fg: too many arguments");
            return 1;
        }
        if (!jobs_get_by_spec(argv[1], &job)) {
            posish_errorf("fg: no such job: %s", argv[1]);
            return 1;
        }
    } else {
        if (!jobs_get_current(true, &job) && !jobs_get_current(false, &job)) {
            posish_errorf("fg: no current job");
            return 1;
        }
    }

    if (job.command != NULL && job.command[0] != '\0') {
        puts(job.command);
        fflush(stdout);
    }
    return wait_foreground_job(state, &job);
}

static int builtin_bg(struct shell_state *state, char *const argv[]) {
    size_t i;
    int status;

    if (!state->monitor_mode) {
        posish_errorf("bg: job control is disabled");
        return 1;
    }

    status = 0;
    if (argv[1] == NULL) {
        struct jobs_entry_info job;

        if (!jobs_get_current(true, &job) && !jobs_get_current(false, &job)) {
            posish_errorf("bg: no current job");
            return 1;
        }

        if (job.stopped) {
            pid_t job_pgid;

            job_pgid = job.pgid > 0 ? job.pgid : job.pid;
            jobs_mark_running(job.pid);
            if (kill(-job_pgid, SIGCONT) != 0) {
                perror("bg");
                return 1;
            }
        }
        if (job.command != NULL && job.command[0] != '\0') {
            printf("[%u] %s\n", job.job_id, job.command);
        } else {
            printf("[%u] %ld\n", job.job_id, (long)job.pid);
        }
        fflush(stdout);
        state->last_async_pid = job.pid;
        return 0;
    }

    for (i = 1; argv[i] != NULL; i++) {
        struct jobs_entry_info job;

        if (!jobs_get_by_spec(argv[i], &job)) {
            posish_errorf("bg: no such job: %s", argv[i]);
            status = 1;
            continue;
        }

        if (job.stopped) {
            pid_t job_pgid;

            job_pgid = job.pgid > 0 ? job.pgid : job.pid;
            jobs_mark_running(job.pid);
            if (kill(-job_pgid, SIGCONT) != 0) {
                perror("bg");
                status = 1;
                continue;
            }
        }
        if (job.command != NULL && job.command[0] != '\0') {
            printf("[%u] %s\n", job.job_id, job.command);
        } else {
            printf("[%u] %ld\n", job.job_id, (long)job.pid);
        }
        fflush(stdout);
        state->last_async_pid = job.pid;
    }

    return status;
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
        return builtin_fg(state, argv);
    }
    if (strcmp(argv[0], "bg") == 0) {
        *handled = true;
        return builtin_bg(state, argv);
    }
    if (strcmp(argv[0], "umask") == 0) {
        *handled = true;
        return builtin_umask(argv);
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
    static const char *const regular_names[] = {"cd",       "true",        "false",
                                                "test",     "[",           "kill",
                                                "wait",     "fg",          "bg",
                                                "umask",
                                                "alias",    "unalias",     "echoraw",
                                                "bracket",  "make_command"};
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
