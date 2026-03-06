/* SPDX-License-Identifier: 0BSD */

/* posish - native test/[ builtin */

#include "builtins/test.h"

#include "compat.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

struct test_parser {
    char *const *argv;
    int argc;
    int pos;
    const char *error_token;
    const char *error_message;
    bool have_error;
};

static void test_errorf(const char *fmt, ...) __printflike(1, 2);

static void test_errorf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    fputs("test: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void parser_syntax_error(struct test_parser *p, const char *token,
                                const char *message)
{
    if (p->have_error) {
        return;
    }
    p->have_error = true;
    p->error_token = token;
    p->error_message = message;
}

static const char *parser_peek(const struct test_parser *p)
{
    if (p->pos >= p->argc) {
        return NULL;
    }
    return p->argv[p->pos];
}

static bool is_unary_operator(const char *s)
{
    static const char *const unary_ops[] = {
        "-b", "-c", "-d", "-e", "-f", "-g", "-h", "-k",
        "-L", "-n", "-O", "-G", "-p", "-r", "-S", "-s",
        "-t", "-u", "-w", "-x", "-z"
    };
    size_t i;

    for (i = 0; i < sizeof(unary_ops) / sizeof(unary_ops[0]); i++) {
        if (strcmp(s, unary_ops[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool is_binary_operator(const char *s)
{
    static const char *const binary_ops[] = {
        "=", "!=", "<", ">",
        "-eq", "-ne", "-gt", "-ge", "-lt", "-le",
        "-nt", "-ot", "-ef"
    };
    size_t i;

    for (i = 0; i < sizeof(binary_ops) / sizeof(binary_ops[0]); i++) {
        if (strcmp(s, binary_ops[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool parse_integer_operand(const char *text, intmax_t *out)
{
    char *end;
    intmax_t v;

    if (text == NULL || text[0] == '\0') {
        return false;
    }

    errno = 0;
    v = strtoimax(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }
    *out = v;
    return true;
}

static bool file_stat(const char *path, bool follow_links, struct stat *st)
{
    if (follow_links) {
        return stat(path, st) == 0;
    }
    return lstat(path, st) == 0;
}

static int eval_unary(const char *op, const char *arg, bool *out)
{
    struct stat st;
    intmax_t fdnum;

    if (strcmp(op, "-n") == 0) {
        *out = arg[0] != '\0';
        return 0;
    }
    if (strcmp(op, "-z") == 0) {
        *out = arg[0] == '\0';
        return 0;
    }
    if (strcmp(op, "-r") == 0) {
        *out = access(arg, R_OK) == 0;
        return 0;
    }
    if (strcmp(op, "-w") == 0) {
        *out = access(arg, W_OK) == 0;
        return 0;
    }
    if (strcmp(op, "-x") == 0) {
        *out = access(arg, X_OK) == 0;
        return 0;
    }
    if (strcmp(op, "-t") == 0) {
        if (!parse_integer_operand(arg, &fdnum) || fdnum < 0) {
            *out = false;
            return 0;
        }
        *out = isatty((int)fdnum) != 0;
        return 0;
    }
    if (strcmp(op, "-h") == 0 || strcmp(op, "-L") == 0) {
        *out = file_stat(arg, false, &st) && S_ISLNK(st.st_mode);
        return 0;
    }
    if (!file_stat(arg, true, &st)) {
        *out = false;
        return 0;
    }
    if (strcmp(op, "-e") == 0) {
        *out = true;
    } else if (strcmp(op, "-b") == 0) {
        *out = S_ISBLK(st.st_mode);
    } else if (strcmp(op, "-c") == 0) {
        *out = S_ISCHR(st.st_mode);
    } else if (strcmp(op, "-d") == 0) {
        *out = S_ISDIR(st.st_mode);
    } else if (strcmp(op, "-f") == 0) {
        *out = S_ISREG(st.st_mode);
    } else if (strcmp(op, "-g") == 0) {
        *out = (st.st_mode & S_ISGID) != 0;
    } else if (strcmp(op, "-k") == 0) {
        *out = (st.st_mode & S_ISVTX) != 0;
    } else if (strcmp(op, "-O") == 0) {
        *out = st.st_uid == geteuid();
    } else if (strcmp(op, "-G") == 0) {
        *out = st.st_gid == getegid();
    } else if (strcmp(op, "-p") == 0) {
        *out = S_ISFIFO(st.st_mode);
    } else if (strcmp(op, "-S") == 0) {
        *out = S_ISSOCK(st.st_mode);
    } else if (strcmp(op, "-s") == 0) {
        *out = st.st_size > 0;
    } else if (strcmp(op, "-u") == 0) {
        *out = (st.st_mode & S_ISUID) != 0;
    } else {
        *out = false;
    }
    return 0;
}

static int eval_binary(const char *left, const char *op, const char *right,
                       bool *out)
{
    intmax_t lnum;
    intmax_t rnum;
    struct stat lst;
    struct stat rst;
    bool have_l;
    bool have_r;

    if (strcmp(op, "=") == 0) {
        *out = strcmp(left, right) == 0;
        return 0;
    }
    if (strcmp(op, "!=") == 0) {
        *out = strcmp(left, right) != 0;
        return 0;
    }
    if (strcmp(op, "<") == 0) {
        *out = strcmp(left, right) < 0;
        return 0;
    }
    if (strcmp(op, ">") == 0) {
        *out = strcmp(left, right) > 0;
        return 0;
    }

    if (strcmp(op, "-eq") == 0 || strcmp(op, "-ne") == 0 ||
        strcmp(op, "-gt") == 0 || strcmp(op, "-ge") == 0 ||
        strcmp(op, "-lt") == 0 || strcmp(op, "-le") == 0) {
        if (!parse_integer_operand(left, &lnum) ||
            !parse_integer_operand(right, &rnum)) {
            test_errorf("integer expression expected");
            return 2;
        }
        if (strcmp(op, "-eq") == 0) {
            *out = lnum == rnum;
        } else if (strcmp(op, "-ne") == 0) {
            *out = lnum != rnum;
        } else if (strcmp(op, "-gt") == 0) {
            *out = lnum > rnum;
        } else if (strcmp(op, "-ge") == 0) {
            *out = lnum >= rnum;
        } else if (strcmp(op, "-lt") == 0) {
            *out = lnum < rnum;
        } else {
            *out = lnum <= rnum;
        }
        return 0;
    }

    have_l = file_stat(left, true, &lst);
    have_r = file_stat(right, true, &rst);

    if (strcmp(op, "-ef") == 0) {
        *out = have_l && have_r && lst.st_dev == rst.st_dev &&
               lst.st_ino == rst.st_ino;
        return 0;
    }

    if (!have_l && !have_r) {
        *out = false;
        return 0;
    }
    if (!have_l) {
        *out = strcmp(op, "-ot") == 0 && have_r;
        return 0;
    }
    if (!have_r) {
        *out = strcmp(op, "-nt") == 0 && have_l;
        return 0;
    }
    if (strcmp(op, "-nt") == 0) {
        *out = timespeccmp(&lst.st_mtim, &rst.st_mtim, >);
    } else {
        *out = timespeccmp(&lst.st_mtim, &rst.st_mtim, <);
    }
    return 0;
}

static int eval_one_arg(const char *arg)
{
    return arg[0] == '\0' ? 1 : 0;
}

static int eval_two_arg(const char *a1, const char *a2)
{
    bool value;
    int status;

    if (strcmp(a1, "!") == 0) {
        return eval_one_arg(a2) == 0 ? 1 : 0;
    }
    if (is_unary_operator(a1)) {
        status = eval_unary(a1, a2, &value);
        if (status != 0) {
            return status;
        }
        return value ? 0 : 1;
    }
    if (strcmp(a1, "(") == 0 && strcmp(a2, ")") == 0) {
        return 1;
    }
    return -1;
}

static int eval_three_arg(const char *a1, const char *a2, const char *a3)
{
    bool value;
    int status;

    if (is_binary_operator(a2)) {
        status = eval_binary(a1, a2, a3, &value);
        if (status != 0) {
            return status;
        }
        return value ? 0 : 1;
    }
    if (strcmp(a1, "!") == 0) {
        status = eval_two_arg(a2, a3);
        if (status < 0 || status == 2) {
            return status;
        }
        return status == 0 ? 1 : 0;
    }
    if (strcmp(a1, "(") == 0 && strcmp(a3, ")") == 0) {
        return eval_one_arg(a2);
    }
    return -1;
}

static int eval_four_arg(const char *a1, const char *a2,
                         const char *a3, const char *a4)
{
    int status;

    if (strcmp(a1, "!") == 0) {
        status = eval_three_arg(a2, a3, a4);
        if (status < 0 || status == 2) {
            return status;
        }
        return status == 0 ? 1 : 0;
    }
    if (strcmp(a1, "(") == 0 && strcmp(a4, ")") == 0) {
        return eval_two_arg(a2, a3);
    }
    return -1;
}

static int parse_oexpr(struct test_parser *p, bool *out);

static int parse_primary(struct test_parser *p, bool *out)
{
    const char *tok;
    const char *next;
    bool value;
    int status;

    tok = parser_peek(p);
    if (tok == NULL) {
        parser_syntax_error(p, NULL, "argument expected");
        return 2;
    }

    if (strcmp(tok, "(") == 0) {
        p->pos++;
        status = parse_oexpr(p, out);
        if (status != 0) {
            return status;
        }
        tok = parser_peek(p);
        if (tok == NULL || strcmp(tok, ")") != 0) {
            parser_syntax_error(p, tok, "missing )");
            return 2;
        }
        p->pos++;
        return 0;
    }

    next = (p->pos + 1 < p->argc) ? p->argv[p->pos + 1] : NULL;
    if (is_unary_operator(tok) && next != NULL) {
        p->pos += 2;
        status = eval_unary(tok, next, &value);
        if (status != 0) {
            return status;
        }
        *out = value;
        return 0;
    }

    if (p->pos + 2 < p->argc && is_binary_operator(p->argv[p->pos + 1])) {
        const char *left;
        const char *op;
        const char *right;

        left = p->argv[p->pos];
        op = p->argv[p->pos + 1];
        right = p->argv[p->pos + 2];
        p->pos += 3;

        status = eval_binary(left, op, right, &value);
        if (status != 0) {
            return status;
        }
        *out = value;
        return 0;
    }

    p->pos++;
    *out = tok[0] != '\0';
    return 0;
}

static int parse_nexpr(struct test_parser *p, bool *out)
{
    const char *tok;
    int status;

    tok = parser_peek(p);
    if (tok != NULL && strcmp(tok, "!") == 0) {
        p->pos++;
        status = parse_nexpr(p, out);
        if (status != 0) {
            return status;
        }
        *out = !*out;
        return 0;
    }
    return parse_primary(p, out);
}

static int parse_aexpr(struct test_parser *p, bool *out)
{
    bool rhs;
    const char *tok;
    int status;

    status = parse_nexpr(p, out);
    if (status != 0) {
        return status;
    }

    for (;;) {
        tok = parser_peek(p);
        if (tok == NULL || strcmp(tok, "-a") != 0) {
            break;
        }
        p->pos++;
        status = parse_nexpr(p, &rhs);
        if (status != 0) {
            return status;
        }
        *out = *out && rhs;
    }
    return 0;
}

static int parse_oexpr(struct test_parser *p, bool *out)
{
    bool rhs;
    const char *tok;
    int status;

    status = parse_aexpr(p, out);
    if (status != 0) {
        return status;
    }

    for (;;) {
        tok = parser_peek(p);
        if (tok == NULL || strcmp(tok, "-o") != 0) {
            break;
        }
        p->pos++;
        status = parse_aexpr(p, &rhs);
        if (status != 0) {
            return status;
        }
        *out = *out || rhs;
    }
    return 0;
}

int posish_test_builtin(int argc, char *const argv[])
{
    struct test_parser p;
    int n;
    int res;
    bool value;
    char *const *args;

    if (argc <= 0 || argv == NULL || argv[0] == NULL) {
        return 1;
    }

    if (strcmp(argv[0], "[") == 0) {
        if (argc <= 1 || strcmp(argv[argc - 1], "]") != 0) {
            test_errorf("missing ]");
            return 2;
        }
        args = &argv[1];
        n = argc - 2;
    } else {
        args = &argv[1];
        n = argc - 1;
    }

    if (n == 0) {
        return 1;
    }
    if (n == 1) {
        return eval_one_arg(args[0]);
    }
    if (n == 2) {
        res = eval_two_arg(args[0], args[1]);
        if (res >= 0) {
            return res;
        }
    } else if (n == 3) {
        res = eval_three_arg(args[0], args[1], args[2]);
        if (res >= 0) {
            return res;
        }
    } else if (n == 4) {
        res = eval_four_arg(args[0], args[1], args[2], args[3]);
        if (res >= 0) {
            return res;
        }
    }

    p.argv = args;
    p.argc = n;
    p.pos = 0;
    p.error_token = NULL;
    p.error_message = NULL;
    p.have_error = false;

    if (parse_oexpr(&p, &value) != 0) {
        if (p.have_error) {
            if (p.error_token != NULL && p.error_token[0] != '\0') {
                test_errorf("%s: %s", p.error_token, p.error_message);
            } else {
                test_errorf("%s", p.error_message);
            }
        }
        return 2;
    }

    if (p.pos < p.argc) {
        test_errorf("%s: unexpected operator", p.argv[p.pos]);
        return 2;
    }

    return value ? 0 : 1;
}
