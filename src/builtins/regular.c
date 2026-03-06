/* SPDX-License-Identifier: 0BSD */

/* posish - regular builtins */

#include "builtins/builtin.h"
#include "builtins/test.h"

#include "arena.h"
#include "error.h"
#include "jobs.h"
#include "path.h"
#include "vars.h"

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

static size_t getopts_nextchar_index = 0;
static unsigned long getopts_last_optind = 1;
static char *getopts_last_optstring = NULL;

static char *xstrdup_heap(const char *s) {
    size_t n;
    char *copy;

    n = strlen(s) + 1;
    copy = arena_alloc_in(NULL, n);
    memcpy(copy, s, n);
    return copy;
}

static int wait_status_to_shell_status(int status) {
    return shell_status_from_wait_status(status);
}

static bool cd_operand_ignores_cdpath(const char *operand) {
    if (operand == NULL || operand[0] == '\0') {
        return true;
    }
    if (operand[0] == '/') {
        return true;
    }
    if (operand[0] == '.' &&
        (operand[1] == '\0' || operand[1] == '/' ||
         (operand[1] == '.' && (operand[2] == '\0' || operand[2] == '/')))) {
        return true;
    }
    return false;
}

static char *cd_join_path(const char *left, const char *right) {
    size_t llen;
    size_t rlen;
    bool need_slash;
    char *out;

    llen = strlen(left);
    rlen = strlen(right);
    need_slash = llen > 0 && left[llen - 1] != '/';

    out = arena_xmalloc(llen + (need_slash ? 1 : 0) + rlen + 1);

    memcpy(out, left, llen);
    if (need_slash) {
        out[llen++] = '/';
    }
    memcpy(out + llen, right, rlen + 1);
    return out;
}

static char *cd_canonicalize_logical(const char *base, const char *path) {
    char *joined;
    size_t i;
    size_t seg_count;
    char **segments;
    char *cursor;
    char *out;
    size_t out_len;

    if (path[0] == '/') {
        joined = arena_xstrdup(path);
    } else {
        joined = cd_join_path(base, path);
    }
    if (joined == NULL) {
        return NULL;
    }

    seg_count = 0;
    segments = NULL;
    cursor = joined;
    while (*cursor != '\0') {
        char *start;
        char *end;
        size_t len;
        char *segment;

        while (*cursor == '/') {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }

        start = cursor;
        end = start;
        while (*end != '\0' && *end != '/') {
            end++;
        }
        len = (size_t)(end - start);

        if (len == 1 && start[0] == '.') {
            cursor = end;
            continue;
        }
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            if (seg_count > 0) {
                arena_maybe_free(segments[seg_count - 1]);
                seg_count--;
            }
            cursor = end;
            continue;
        }

        segment = arena_xmalloc(len + 1);
        memcpy(segment, start, len);
        segment[len] = '\0';

        segments = arena_xrealloc(segments, sizeof(*segments) * (seg_count + 1));
        segments[seg_count++] = segment;
        cursor = end;
    }

    out_len = 1;
    for (i = 0; i < seg_count; i++) {
        out_len += strlen(segments[i]) + 1;
    }
    out = arena_xmalloc(out_len + 1);
    out[0] = '/';
    out[1] = '\0';
    out_len = 1;
    for (i = 0; i < seg_count; i++) {
        size_t len;

        len = strlen(segments[i]);
        memcpy(out + out_len, segments[i], len);
        out_len += len;
        if (i + 1 < seg_count) {
            out[out_len++] = '/';
        }
        out[out_len] = '\0';
    }

    for (i = 0; i < seg_count; i++) {
        arena_maybe_free(segments[i]);
    }
    arena_maybe_free(segments);
    arena_maybe_free(joined);
    return out;
}

static int builtin_cd(struct shell_state *state, char *const argv[]) {
    size_t i;
    bool physical;
    bool opt_e;
    const char *operand;
    const char *target;
    char *old_pwd;
    char *resolved_path;
    bool print_path;
    bool used_cdpath;
    int failure_status;
    char *new_pwd;
    char *logical_target;

    i = 1;
    physical = false;
    opt_e = false;
    while (argv[i] != NULL) {
        size_t j;

        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (argv[i][0] != '-' || argv[i][1] == '\0' ||
            (argv[i][1] == '-' && argv[i][2] == '\0')) {
            break;
        }

        for (j = 1; argv[i][j] != '\0'; j++) {
            if (argv[i][j] == 'L') {
                physical = false;
                continue;
            }
            if (argv[i][j] == 'P') {
                physical = true;
                continue;
            }
            if (argv[i][j] == 'e') {
                opt_e = true;
                continue;
            }
            posish_errorf("cd: invalid option: -%c", argv[i][j]);
            return 2;
        }
        i++;
    }

    if (argv[i] != NULL && argv[i + 1] != NULL) {
        posish_errorf("cd: too many arguments");
        return 1;
    }

    operand = argv[i];
    print_path = false;
    if (operand == NULL) {
        target = getenv("HOME");
        if (target == NULL || target[0] == '\0') {
            posish_errorf("cd: HOME is not set");
            return 1;
        }
    } else if (strcmp(operand, "-") == 0) {
        target = getenv("OLDPWD");
        if (target == NULL || target[0] == '\0') {
            posish_errorf("cd: OLDPWD is not set");
            return 1;
        }
        print_path = true;
    } else {
        target = operand;
    }

    old_pwd = getenv("PWD") != NULL ? arena_xstrdup(getenv("PWD"))
                                     : path_getcwd_alloc();
    if (old_pwd == NULL) {
        old_pwd = arena_xstrdup("/");
    }

    resolved_path = arena_xstrdup(target);
    used_cdpath = false;
    if (!cd_operand_ignores_cdpath(target)) {
        const char *cdpath;

        cdpath = getenv("CDPATH");
        if (cdpath != NULL) {
            const char *p;
            bool found_path;

            p = cdpath;
            found_path = false;
            while (!found_path) {
                const char *end;
                size_t len;
                char *prefix;
                char *candidate;
                struct stat st;

                end = strchr(p, ':');
                if (end == NULL) {
                    end = p + strlen(p);
                }
                len = (size_t)(end - p);
                prefix = arena_xmalloc(len + 1);
                memcpy(prefix, p, len);
                prefix[len] = '\0';

                if (len == 0) {
                    candidate = arena_xstrdup(target);
                } else {
                    candidate = cd_join_path(prefix, target);
                }
                if (candidate == NULL) {
                    arena_maybe_free(prefix);
                    arena_maybe_free(old_pwd);
                    arena_maybe_free(resolved_path);
                    return 1;
                }

                if (stat(candidate, &st) == 0 && S_ISDIR(st.st_mode)) {
                    arena_maybe_free(resolved_path);
                    resolved_path = candidate;
                    if (len > 0) {
                        print_path = true;
                    }
                    used_cdpath = true;
                    found_path = true;
                } else {
                    arena_maybe_free(candidate);
                }

                arena_maybe_free(prefix);
                if (found_path || *end == '\0') {
                    break;
                }
                p = end + 1;
            }
        }
    }

    failure_status = opt_e ? 2 : 1;
    logical_target = NULL;
    if (physical) {
        if (chdir(used_cdpath ? resolved_path : target) != 0) {
            perror("cd");
            arena_maybe_free(old_pwd);
            arena_maybe_free(resolved_path);
            return failure_status;
        }
        new_pwd = path_getcwd_alloc();
        if (new_pwd == NULL) {
            posish_errorf("cd: failed to determine current directory");
            arena_maybe_free(old_pwd);
            arena_maybe_free(resolved_path);
            return failure_status;
        }
    } else {
        const char *raw_target;

        raw_target = used_cdpath ? resolved_path : target;
        logical_target = cd_canonicalize_logical(
            old_pwd, raw_target);
        if (logical_target == NULL) {
            posish_errorf("cd: failed to build logical path");
            arena_maybe_free(old_pwd);
            arena_maybe_free(resolved_path);
            return failure_status;
        }
        /*
         * Validate component traversal on the raw operand first. This keeps
         * `file/../dir` failures visible in -L mode before logical rewrite.
         */
        if (chdir(raw_target) != 0) {
            perror("cd");
            arena_maybe_free(old_pwd);
            arena_maybe_free(resolved_path);
            arena_maybe_free(logical_target);
            return failure_status;
        }
        if (strcmp(logical_target, raw_target) != 0 &&
            chdir(logical_target) != 0) {
            perror("cd");
            arena_maybe_free(old_pwd);
            arena_maybe_free(resolved_path);
            arena_maybe_free(logical_target);
            return failure_status;
        }
        new_pwd = logical_target;
        logical_target = NULL;
    }

    if (vars_set(state, "OLDPWD", old_pwd, true) != 0) {
        arena_maybe_free(old_pwd);
        arena_maybe_free(resolved_path);
        arena_maybe_free(logical_target);
        arena_maybe_free(new_pwd);
        return 1;
    }
    if (vars_set(state, "PWD", new_pwd, true) != 0) {
        arena_maybe_free(old_pwd);
        arena_maybe_free(resolved_path);
        arena_maybe_free(logical_target);
        arena_maybe_free(new_pwd);
        return 1;
    }

    if (print_path) {
        puts(new_pwd);
    }

    arena_maybe_free(old_pwd);
    arena_maybe_free(resolved_path);
    arena_maybe_free(logical_target);
    arena_maybe_free(new_pwd);
    return 0;
}

static int builtin_pwd(char *const argv[]) {
    size_t i;
    bool physical;
    char *cwd;
    const char *pwd;

    i = 1;
    physical = false;
    while (argv[i] != NULL) {
        size_t j;

        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (argv[i][0] != '-' || argv[i][1] == '\0' ||
            (argv[i][1] == '-' && argv[i][2] == '\0')) {
            break;
        }

        for (j = 1; argv[i][j] != '\0'; j++) {
            if (argv[i][j] == 'L') {
                physical = false;
                continue;
            }
            if (argv[i][j] == 'P') {
                physical = true;
                continue;
            }
            posish_errorf("pwd: invalid option: -%c", argv[i][j]);
            return 2;
        }
        i++;
    }

    if (argv[i] != NULL) {
        posish_errorf("pwd: too many arguments");
        return 1;
    }

    pwd = getenv("PWD");
    if (!physical && pwd != NULL && pwd[0] == '/') {
        puts(pwd);
        return 0;
    }

    cwd = path_getcwd_alloc();
    if (cwd == NULL) {
        perror("pwd");
        return 1;
    }

    puts(cwd);
    arena_maybe_free(cwd);
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
    return posish_test_builtin((int)argc, argv);
}

#if POSISH_TEST_HELPERS
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
#endif

static char *pid_to_string(pid_t pid) {
    char buf[32];
    int len;
    char *out;

    len = snprintf(buf, sizeof(buf), "%ld", (long)pid);
    if (len < 0 || (size_t)len >= sizeof(buf)) {
        return NULL;
    }

    out = arena_xmalloc((size_t)len + 1);
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
    size_t operand_start;
    bool insert_double_dash;
    bool had_jobspec;
    bool list_mode;
    int status;

    argc = 0;
    while (argv[argc] != NULL) {
        argc++;
    }

    converted = arena_xmalloc(sizeof(*converted) * (argc + 1));
    memset(converted, 0, sizeof(*converted) * (argc + 1));
    final_argv = NULL;
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
                int signo;
                char *status_text;

                if (shell_status_signal_number((int)value, &signo)) {
                    status_text = pid_to_string((pid_t)signo);
                } else {
                    status_text = pid_to_string((pid_t)(value - 128));
                }
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
            enum jobs_lookup_result lookup;
            char *pid_text;

            lookup = jobs_get_by_spec(argv[i], &job);
            if (lookup != JOBS_LOOKUP_OK || job.pgid <= 0) {
                if (lookup == JOBS_LOOKUP_AMBIGUOUS) {
                    posish_errorf("kill: ambiguous job: %s", argv[i]);
                } else if (lookup == JOBS_LOOKUP_INVALID) {
                    posish_errorf("kill: invalid job spec: %s", argv[i]);
                } else {
                    posish_errorf("kill: no such job: %s", argv[i]);
                }
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

        final_argv = arena_xmalloc(sizeof(*final_argv) * (argc + 2));
        memset(final_argv, 0, sizeof(*final_argv) * (argc + 2));
        for (j = 0; j < operand_start; j++) {
            final_argv[j] = converted[j];
        }
        final_argv[operand_start] = "--";
        for (j = operand_start; j < argc; j++) {
            final_argv[j + 1] = converted[j];
        }
        final_argv[argc + 1] = NULL;
    }

    status = run_utility(final_argv != NULL ? final_argv : converted);

done:
    for (i = 0; i < argc; i++) {
        if (converted[i] != NULL && converted[i] != argv[i]) {
            arena_maybe_free(converted[i]);
        }
    }
    if (final_argv != NULL) {
        arena_maybe_free(final_argv);
    }
    arena_maybe_free(converted);
    return status;
}

static bool alias_name_valid(const char *name) {
    size_t i;

    if (name == NULL || name[0] == '\0') {
        return false;
    }
    for (i = 0; name[i] != '\0'; i++) {
        if (name[i] == '=' || isspace((unsigned char)name[i])) {
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
    key = arena_xmalloc(plen + nlen + 1);
    memcpy(key, POSISH_ALIAS_ENV_PREFIX, plen);
    memcpy(key + plen, name, nlen + 1);
    return key;
}

static char *alias_quote_value(const char *value) {
    size_t i;
    size_t out_len;
    char *out;
    size_t pos;

    out_len = 2;
    for (i = 0; value[i] != '\0'; i++) {
        if (value[i] == '\'') {
            out_len += 4;
        } else {
            out_len++;
        }
    }

    out = arena_xmalloc(out_len + 1);

    pos = 0;
    out[pos++] = '\'';
    for (i = 0; value[i] != '\0'; i++) {
        if (value[i] == '\'') {
            out[pos++] = '\'';
            out[pos++] = '\\';
            out[pos++] = '\'';
            out[pos++] = '\'';
        } else {
            out[pos++] = value[i];
        }
    }
    out[pos++] = '\'';
    out[pos] = '\0';
    return out;
}

static int alias_print_entry(const char *name, const char *value) {
    char *quoted;

    quoted = alias_quote_value(value);
    if (quoted == NULL) {
        return 1;
    }
    printf("%s=%s\n", name, quoted);
    arena_maybe_free(quoted);
    return 0;
}

static int builtin_alias(char *const argv[]) {
    int status;
    size_t i;

    i = 1;
    if (argv[i] != NULL && strcmp(argv[i], "--") == 0) {
        i++;
    }

    if (argv[i] == NULL) {
        size_t prefix_len;
        size_t k;

        prefix_len = strlen(POSISH_ALIAS_ENV_PREFIX);
        for (k = 0; environ[k] != NULL; k++) {
            const char *eq;
            const char *name;
            const char *value;
            size_t name_len;
            char *name_buf;

            if (strncmp(environ[k], POSISH_ALIAS_ENV_PREFIX, prefix_len) != 0) {
                continue;
            }

            eq = strchr(environ[k], '=');
            if (eq == NULL || (size_t)(eq - environ[k]) <= prefix_len) {
                continue;
            }

            name = environ[k] + prefix_len;
            name_len = (size_t)(eq - name);
            value = eq + 1;

            name_buf = arena_xmalloc(name_len + 1);
            memcpy(name_buf, name, name_len);
            name_buf[name_len] = '\0';

            if (alias_print_entry(name_buf, value) != 0) {
                arena_maybe_free(name_buf);
                return 1;
            }
            arena_maybe_free(name_buf);
        }
        return 0;
    }

    status = 0;
    for (; argv[i] != NULL; i++) {
        char *eq;
        char *name;
        char *key;
        const char *value;

        eq = strchr(argv[i], '=');
        if (eq == NULL) {
            if (!alias_name_valid(argv[i])) {
                posish_errorf("alias: invalid name: %s", argv[i]);
                status = 1;
                continue;
            }

            key = alias_env_key(argv[i]);
            if (key == NULL) {
                return 1;
            }
            value = getenv(key);
            if (value == NULL) {
                posish_errorf("alias: %s: not found", argv[i]);
                arena_maybe_free(key);
                status = 1;
                continue;
            }
            if (alias_print_entry(argv[i], value) != 0) {
                arena_maybe_free(key);
                return 1;
            }
            arena_maybe_free(key);
            continue;
        }

        name = arena_xmalloc((size_t)(eq - argv[i]) + 1);
        memcpy(name, argv[i], (size_t)(eq - argv[i]));
        name[eq - argv[i]] = '\0';
        value = eq + 1;

        if (!alias_name_valid(name)) {
            posish_errorf("alias: invalid name: %s", name);
            arena_maybe_free(name);
            status = 1;
            continue;
        }

        key = alias_env_key(name);
        if (key == NULL) {
            arena_maybe_free(name);
            return 1;
        }
        if (setenv(key, value, 1) != 0) {
            perror("setenv");
            arena_maybe_free(key);
            arena_maybe_free(name);
            return 1;
        }

        arena_maybe_free(key);
        arena_maybe_free(name);
    }

    return status;
}

static int builtin_unalias(char *const argv[]) {
    int status;
    size_t i;
    bool clear_all;
    size_t prefix_len;

    clear_all = false;
    i = 1;
    while (argv[i] != NULL && argv[i][0] == '-' && argv[i][1] != '\0') {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (strcmp(argv[i], "-a") == 0) {
            clear_all = true;
            i++;
            continue;
        }
        posish_errorf("unalias: invalid option: %s", argv[i]);
        return 2;
    }

    if (!clear_all && argv[i] == NULL) {
        posish_errorf("unalias: missing operand");
        return 1;
    }

    status = 0;
    prefix_len = strlen(POSISH_ALIAS_ENV_PREFIX);

    if (clear_all) {
        char **keys;
        size_t key_count;
        size_t k;

        keys = NULL;
        key_count = 0;
        for (k = 0; environ[k] != NULL; k++) {
            const char *eq;
            size_t len;
            char *key;

            if (strncmp(environ[k], POSISH_ALIAS_ENV_PREFIX, prefix_len) != 0) {
                continue;
            }
            eq = strchr(environ[k], '=');
            if (eq == NULL) {
                continue;
            }

            len = (size_t)(eq - environ[k]);
            key = arena_xmalloc(len + 1);
            memcpy(key, environ[k], len);
            key[len] = '\0';
            keys = arena_xrealloc(keys, sizeof(*keys) * (key_count + 1));
            keys[key_count++] = key;
        }

        for (k = 0; k < key_count; k++) {
            if (unsetenv(keys[k]) != 0) {
                perror("unsetenv");
                status = 1;
            }
            arena_maybe_free(keys[k]);
        }
        arena_maybe_free(keys);
    }

    for (; argv[i] != NULL; i++) {
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
        if (getenv(key) == NULL) {
            posish_errorf("unalias: %s: not found", argv[i]);
            arena_maybe_free(key);
            status = 1;
            continue;
        }
        if (unsetenv(key) != 0) {
            perror("unsetenv");
            arena_maybe_free(key);
            return 1;
        }
        arena_maybe_free(key);
    }
    return status;
}

static int getopts_set_var(struct shell_state *state, const char *name,
                           const char *value) {
    return vars_set_with_mode(state, name, value, true, false);
}

static int getopts_unset_var(struct shell_state *state, const char *name) {
    return vars_unset(state, name);
}

static int getopts_set_char_var(struct shell_state *state, const char *name,
                                char ch) {
    char value[2];

    value[0] = ch;
    value[1] = '\0';
    return getopts_set_var(state, name, value);
}

static unsigned long getopts_read_optind(void) {
    const char *value;
    char *end;
    unsigned long parsed;

    value = getenv("OPTIND");
    if (value == NULL || value[0] == '\0') {
        return 1;
    }

    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0) {
        return 1;
    }
    return parsed;
}

static int builtin_getopts(struct shell_state *state, char *const argv[]) {
    const char *optstring;
    const char *varname;
    char *const *arg_list;
    size_t arg_count;
    bool silent_mode;
    unsigned long optind;
    const char *current;
    char optch;
    const char *spec;
    const char *match;
    char optind_text[32];

    if (argv[1] == NULL || argv[2] == NULL) {
        posish_errorf("getopts: missing operands");
        return 1;
    }

    optstring = argv[1];
    varname = argv[2];
    if (!vars_is_name_valid(varname)) {
        posish_errorf("getopts: invalid variable name: %s", varname);
        return 1;
    }

    if (argv[3] != NULL) {
        size_t i;

        arg_list = argv + 3;
        arg_count = 0;
        for (i = 3; argv[i] != NULL; i++) {
            arg_count++;
        }
    } else {
        arg_list = state->positional_params;
        arg_count = state->positional_count;
    }

    optind = getopts_read_optind();
    if (getopts_last_optstring == NULL ||
        strcmp(getopts_last_optstring, optstring) != 0 ||
        optind != getopts_last_optind) {
        arena_maybe_free(getopts_last_optstring);
        getopts_last_optstring = xstrdup_heap(optstring);
        if (getopts_last_optstring == NULL) {
            return 1;
        }
        getopts_nextchar_index = 0;
    }

    silent_mode = optstring[0] == ':';
    spec = silent_mode ? optstring + 1 : optstring;

    for (;;) {
        if (getopts_nextchar_index == 0) {
            if (optind == 0) {
                optind = 1;
            }
            if (optind > arg_count) {
                goto end_of_options;
            }

            current = arg_list[optind - 1];
            if (current[0] != '-' || current[1] == '\0') {
                goto end_of_options;
            }
            if (strcmp(current, "--") == 0) {
                optind++;
                goto end_of_options;
            }
            getopts_nextchar_index = 1;
        }

        current = arg_list[optind - 1];
        optch = current[getopts_nextchar_index];
        if (optch == '\0') {
            optind++;
            getopts_nextchar_index = 0;
            continue;
        }
        getopts_nextchar_index++;
        break;
    }

    match = strchr(spec, optch);
    if (match == NULL || optch == ':') {
        if (current[getopts_nextchar_index] == '\0') {
            optind++;
            getopts_nextchar_index = 0;
        }

        if (getopts_set_char_var(state, varname, '?') != 0) {
            return 1;
        }
        if (silent_mode) {
            if (getopts_set_char_var(state, "OPTARG", optch) != 0) {
                return 1;
            }
        } else {
            (void)getopts_unset_var(state, "OPTARG");
            posish_errorf("getopts: illegal option -- %c", optch);
        }
    } else if (match[1] == ':') {
        const char *optarg_value;

        if (current[getopts_nextchar_index] != '\0') {
            optarg_value = current + getopts_nextchar_index;
            optind++;
            getopts_nextchar_index = 0;
        } else if (optind < arg_count) {
            optarg_value = arg_list[optind];
            optind += 2;
            getopts_nextchar_index = 0;
        } else {
            optind++;
            getopts_nextchar_index = 0;
            if (silent_mode) {
                if (getopts_set_char_var(state, varname, ':') != 0) {
                    return 1;
                }
                if (getopts_set_char_var(state, "OPTARG", optch) != 0) {
                    return 1;
                }
            } else {
                if (getopts_set_char_var(state, varname, '?') != 0) {
                    return 1;
                }
                (void)getopts_unset_var(state, "OPTARG");
                posish_errorf("getopts: option requires an argument -- %c",
                              optch);
            }
            snprintf(optind_text, sizeof(optind_text), "%lu", optind);
            if (getopts_set_var(state, "OPTIND", optind_text) != 0) {
                return 1;
            }
            getopts_last_optind = optind;
            return 0;
        }

        if (getopts_set_char_var(state, varname, optch) != 0) {
            return 1;
        }
        if (getopts_set_var(state, "OPTARG", optarg_value) != 0) {
            return 1;
        }
    } else {
        if (current[getopts_nextchar_index] == '\0') {
            optind++;
            getopts_nextchar_index = 0;
        }
        if (getopts_set_char_var(state, varname, optch) != 0) {
            return 1;
        }
        if (getopts_unset_var(state, "OPTARG") != 0 &&
            vars_is_readonly(state, "OPTARG")) {
            return 1;
        }
    }

    snprintf(optind_text, sizeof(optind_text), "%lu", optind);
    if (getopts_set_var(state, "OPTIND", optind_text) != 0) {
        return 1;
    }
    getopts_last_optind = optind;
    return 0;

end_of_options:
    if (getopts_set_char_var(state, varname, '?') != 0) {
        return 1;
    }
    if (getopts_unset_var(state, "OPTARG") != 0 &&
        vars_is_readonly(state, "OPTARG")) {
        return 1;
    }
    snprintf(optind_text, sizeof(optind_text), "%lu", optind);
    if (getopts_set_var(state, "OPTIND", optind_text) != 0) {
        return 1;
    }
    getopts_nextchar_index = 0;
    getopts_last_optind = optind;
    return 1;
}

static int builtin_hash(char *const argv[]) {
    size_t i;

    for (i = 1; argv[i] != NULL; i++) {
        if (strcmp(argv[i], "-r") == 0) {
            continue;
        }
        if (argv[i][0] == '-') {
            posish_errorf("hash: invalid option: %s", argv[i]);
            return 2;
        }
    }
    return 0;
}

static int builtin_jobs(char *const argv[]) {
    (void)argv;
    return 0;
}

static int builtin_type(char *const argv[]) {
    size_t i;
    int status;

    if (argv[1] == NULL) {
        posish_errorf("type: missing operand");
        return 1;
    }

    status = 0;
    for (i = 1; argv[i] != NULL; i++) {
        if (builtin_is_name(argv[i])) {
            printf("%s is a shell builtin\n", argv[i]);
            continue;
        }
        if (strchr(argv[i], '/') != NULL) {
            if (access(argv[i], X_OK) == 0) {
                puts(argv[i]);
                continue;
            }
            status = 1;
            continue;
        }
        status = 1;
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

static int wait_for_job(struct shell_state *state,
                        const struct jobs_entry_info *job, int *status_out,
                        int *interrupted_signal_out) {
    pid_t job_pgid;
    int cached_status;

    if (job == NULL || status_out == NULL) {
        errno = EINVAL;
        return -1;
    }

    job_pgid = job->pgid > 0 ? job->pgid : job->status_pid;
    if (job_pgid <= 0) {
        errno = ESRCH;
        return -1;
    }

    if (jobs_job_is_completed(job_pgid)) {
        if (jobs_get_job_wait_status(job_pgid, &cached_status)) {
            *status_out = cached_status;
            return 0;
        }
        errno = ECHILD;
        return -1;
    }

    for (;;) {
        int previous_signal;
        int status;
        pid_t w;
        struct jobs_entry_info current_job;

        previous_signal = state->last_handled_signal;
        w = waitpid(-job_pgid, &status, WUNTRACED);
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

        jobs_note_process_status(w, status);
        if (WIFSTOPPED(status)) {
            if (jobs_get_job_wait_status(job_pgid, &cached_status)) {
                *status_out = cached_status;
            } else {
                *status_out = status;
            }
            return 0;
        }
        if (!jobs_find_by_pgid(job_pgid, &current_job)) {
            *status_out = status;
            return 0;
        }
        if (jobs_job_is_completed(job_pgid)) {
            if (jobs_get_job_wait_status(job_pgid, &cached_status)) {
                *status_out = cached_status;
            } else {
                *status_out = status;
            }
            return 0;
        }
    }
}

static int parse_wait_operand(const char *text, pid_t *pid_out,
                              struct jobs_entry_info *job_out, bool *is_job_out) {
    char *end;
    long n;

    if (pid_out == NULL || job_out == NULL || is_job_out == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (text[0] == '%') {
        enum jobs_lookup_result lookup;

        lookup = jobs_get_by_spec(text, job_out);
        if (lookup == JOBS_LOOKUP_OK) {
            *is_job_out = true;
            return 0;
        }
        if (lookup == JOBS_LOOKUP_NO_MATCH) {
            errno = ESRCH;
        } else {
            errno = EINVAL;
        }
        return -1;
    }

    errno = 0;
    n = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || n <= 0) {
        errno = EINVAL;
        return -1;
    }
    *pid_out = (pid_t)n;
    *is_job_out = false;
    return 0;
}

static void forget_completed_job_by_pid(pid_t pid) {
    struct jobs_entry_info job;

    if (!jobs_find_by_pid(pid, &job)) {
        return;
    }
    if (jobs_job_is_completed(job.pgid)) {
        jobs_forget_pgid(job.pgid);
    }
}

static void report_job_lookup_error(const char *builtin_name, const char *spec,
                                    enum jobs_lookup_result lookup) {
    if (lookup == JOBS_LOOKUP_AMBIGUOUS) {
        posish_errorf("%s: ambiguous job: %s", builtin_name, spec);
    } else if (lookup == JOBS_LOOKUP_INVALID) {
        posish_errorf("%s: invalid job spec: %s", builtin_name, spec);
    } else {
        posish_errorf("%s: no such job: %s", builtin_name, spec);
    }
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

            jobs_note_process_status(w, status);
            forget_completed_job_by_pid(w);
            last_status = wait_status_to_shell_status(status);
        }

        (void)last_status;
        return 0;
    }

    for (i = 1; argv[i] != NULL; i++) {
        pid_t pid;
        int interrupted_signal;
        int status;
        struct jobs_entry_info job;
        bool is_job;

        interrupted_signal = 0;
        if (parse_wait_operand(argv[i], &pid, &job, &is_job) != 0) {
            if (errno == ESRCH) {
                last_status = 127;
                continue;
            }
            posish_errorf("wait: unsupported operand: %s", argv[i]);
            last_status = 1;
            continue;
        }

        if (is_job) {
            if (wait_for_job(state, &job, &status, &interrupted_signal) != 0) {
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
            if (jobs_job_is_completed(job.pgid)) {
                jobs_forget_pgid(job.pgid);
            }
            last_status = wait_status_to_shell_status(status);
            continue;
        }

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

        jobs_note_process_status(pid, status);
        forget_completed_job_by_pid(pid);
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
    job_pgid = job->pgid > 0 ? job->pgid : job->status_pid;
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

    jobs_mark_job_running(job_pgid);
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

    if (wait_for_job(state, job, &status, NULL) != 0) {
        if (transferred_tty) {
            (void)tcsetpgrp(tty_fd, shell_pgid);
        }
        if (close_tty) {
            close(tty_fd);
        }
        perror("waitpid");
        return 1;
    }

    if (transferred_tty) {
        (void)tcsetpgrp(tty_fd, shell_pgid);
    }
    if (close_tty) {
        close(tty_fd);
    }

    if (jobs_job_is_completed(job_pgid)) {
        jobs_forget_pgid(job_pgid);
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
        enum jobs_lookup_result lookup;

        if (argv[2] != NULL) {
            posish_errorf("fg: too many arguments");
            return 1;
        }
        lookup = jobs_get_by_spec(argv[1], &job);
        if (lookup != JOBS_LOOKUP_OK) {
            report_job_lookup_error("fg", argv[1], lookup);
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

            job_pgid = job.pgid > 0 ? job.pgid : job.status_pid;
            jobs_mark_job_running(job_pgid);
            if (kill(-job_pgid, SIGCONT) != 0) {
                perror("bg");
                return 1;
            }
        }
        if (job.command != NULL && job.command[0] != '\0') {
            printf("[%u] %s\n", job.job_id, job.command);
        } else {
            printf("[%u] %ld\n", job.job_id, (long)job.status_pid);
        }
        fflush(stdout);
        state->last_async_pid = job.status_pid;
        return 0;
    }

    for (i = 1; argv[i] != NULL; i++) {
        struct jobs_entry_info job;
        enum jobs_lookup_result lookup;

        lookup = jobs_get_by_spec(argv[i], &job);
        if (lookup != JOBS_LOOKUP_OK) {
            report_job_lookup_error("bg", argv[i], lookup);
            status = 1;
            continue;
        }

        if (job.stopped) {
            pid_t job_pgid;

            job_pgid = job.pgid > 0 ? job.pgid : job.status_pid;
            jobs_mark_job_running(job_pgid);
            if (kill(-job_pgid, SIGCONT) != 0) {
                perror("bg");
                status = 1;
                continue;
            }
        }
        if (job.command != NULL && job.command[0] != '\0') {
            printf("[%u] %s\n", job.job_id, job.command);
        } else {
            printf("[%u] %ld\n", job.job_id, (long)job.status_pid);
        }
        fflush(stdout);
        state->last_async_pid = job.status_pid;
    }

    return status;
}

#if POSISH_TEST_HELPERS
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
#endif

int builtin_dispatch(struct shell_state *state, char *const argv[], bool *handled) {
    int status;

    status = builtin_try_special(state, argv, handled);
    if (*handled) {
        return status;
    }

    if (strcmp(argv[0], "cd") == 0) {
        *handled = true;
        return builtin_cd(state, argv);
    }
    if (strcmp(argv[0], "pwd") == 0) {
        *handled = true;
        return builtin_pwd(argv);
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
    if (strcmp(argv[0], "getopts") == 0) {
        *handled = true;
        return builtin_getopts(state, argv);
    }
    if (strcmp(argv[0], "hash") == 0) {
        *handled = true;
        return builtin_hash(argv);
    }
    if (strcmp(argv[0], "jobs") == 0) {
        *handled = true;
        return builtin_jobs(argv);
    }
    if (strcmp(argv[0], "type") == 0) {
        *handled = true;
        return builtin_type(argv);
    }
    if (strcmp(argv[0], "unalias") == 0) {
        *handled = true;
        return builtin_unalias(argv);
    }
#if POSISH_TEST_HELPERS
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
#endif

    *handled = false;
    return 0;
}

bool builtin_is_name(const char *name) {
    static const char *const regular_names[] = {"cd",     "pwd",    "true",
                                                "false",  "test",   "[",
                                                "kill",   "wait",   "fg",
                                                "bg",     "umask",  "alias",
                                                "command","read",   "getopts",
                                                "hash",   "jobs",   "type",
                                                "unalias"};
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
#if POSISH_TEST_HELPERS
    if (strcmp(name, "echoraw") == 0 || strcmp(name, "bracket") == 0 ||
        strcmp(name, "make_command") == 0) {
        return true;
    }
#endif
    return false;
}

bool builtin_is_substitutive_name(const char *name) {
    return name != NULL && strcmp(name, "pwd") == 0;
}
