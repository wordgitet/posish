#include "arith.h"

#include "error.h"
#include "vars.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum arith_token_kind {
    TOK_END = 0,
    TOK_NUMBER,
    TOK_IDENT,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_QUESTION,
    TOK_COLON,
    TOK_PLUS,
    TOK_MINUS,
    TOK_STAR,
    TOK_SLASH,
    TOK_PERCENT,
    TOK_LT,
    TOK_LE,
    TOK_GT,
    TOK_GE,
    TOK_EQ,
    TOK_NE,
    TOK_AMP,
    TOK_CARET,
    TOK_PIPE,
    TOK_AND_AND,
    TOK_OR_OR,
    TOK_SHL,
    TOK_SHR,
    TOK_TILDE,
    TOK_BANG,
    TOK_ASSIGN,
    TOK_MUL_ASSIGN,
    TOK_DIV_ASSIGN,
    TOK_MOD_ASSIGN,
    TOK_ADD_ASSIGN,
    TOK_SUB_ASSIGN,
    TOK_SHL_ASSIGN,
    TOK_SHR_ASSIGN,
    TOK_AND_ASSIGN,
    TOK_XOR_ASSIGN,
    TOK_OR_ASSIGN
};

struct arith_token {
    enum arith_token_kind kind;
    long number;
    size_t start;
    size_t len;
};

struct arith_parser {
    const char *src;
    size_t pos;
    struct arith_token tok;
    struct shell_state *state;
    bool failed;
};

struct arith_value {
    long value;
    bool assignable;
    size_t name_start;
    size_t name_len;
};

static struct arith_value parse_assignment(struct arith_parser *p, bool eval);

static void parser_fail(struct arith_parser *p, const char *message) {
    if (!p->failed) {
        posish_errorf("%s", message);
        p->failed = true;
    }
}

static bool is_name_start(char ch) {
    return isalpha((unsigned char)ch) || ch == '_';
}

static bool is_name_char(char ch) {
    return isalnum((unsigned char)ch) || ch == '_';
}

static void skip_spaces(struct arith_parser *p) {
    while (p->src[p->pos] != '\0' &&
           isspace((unsigned char)p->src[p->pos])) {
        p->pos++;
    }
}

static bool starts_with(const char *s, const char *op) {
    size_t i;

    for (i = 0; op[i] != '\0'; i++) {
        if (s[i] != op[i]) {
            return false;
        }
    }
    return true;
}

static void next_token(struct arith_parser *p) {
    const char *s;
    char *end;

    skip_spaces(p);
    s = p->src + p->pos;

    p->tok.kind = TOK_END;
    p->tok.number = 0;
    p->tok.start = p->pos;
    p->tok.len = 0;

    if (*s == '\0') {
        return;
    }

    if (isdigit((unsigned char)*s)) {
        errno = 0;
        p->tok.number = strtol(s, &end, 0);
        if (end == s || errno == ERANGE) {
            parser_fail(p, "malformed arithmetic expansion");
            p->tok.kind = TOK_END;
            return;
        }
        p->tok.kind = TOK_NUMBER;
        p->tok.len = (size_t)(end - s);
        p->pos += p->tok.len;
        return;
    }

    if (is_name_start(*s)) {
        size_t len;

        len = 1;
        while (is_name_char(s[len])) {
            len++;
        }

        p->tok.kind = TOK_IDENT;
        p->tok.start = p->pos;
        p->tok.len = len;
        p->pos += len;
        return;
    }

    if (starts_with(s, "<<=")) {
        p->tok.kind = TOK_SHL_ASSIGN;
        p->pos += 3;
        return;
    }
    if (starts_with(s, ">>=")) {
        p->tok.kind = TOK_SHR_ASSIGN;
        p->pos += 3;
        return;
    }

    if (starts_with(s, "&&")) {
        p->tok.kind = TOK_AND_AND;
        p->pos += 2;
        return;
    }
    if (starts_with(s, "||")) {
        p->tok.kind = TOK_OR_OR;
        p->pos += 2;
        return;
    }
    if (starts_with(s, "<<")) {
        p->tok.kind = TOK_SHL;
        p->pos += 2;
        return;
    }
    if (starts_with(s, ">>")) {
        p->tok.kind = TOK_SHR;
        p->pos += 2;
        return;
    }
    if (starts_with(s, "<=")) {
        p->tok.kind = TOK_LE;
        p->pos += 2;
        return;
    }
    if (starts_with(s, ">=")) {
        p->tok.kind = TOK_GE;
        p->pos += 2;
        return;
    }
    if (starts_with(s, "==")) {
        p->tok.kind = TOK_EQ;
        p->pos += 2;
        return;
    }
    if (starts_with(s, "!=")) {
        p->tok.kind = TOK_NE;
        p->pos += 2;
        return;
    }
    if (starts_with(s, "+=")) {
        p->tok.kind = TOK_ADD_ASSIGN;
        p->pos += 2;
        return;
    }
    if (starts_with(s, "-=")) {
        p->tok.kind = TOK_SUB_ASSIGN;
        p->pos += 2;
        return;
    }
    if (starts_with(s, "*=")) {
        p->tok.kind = TOK_MUL_ASSIGN;
        p->pos += 2;
        return;
    }
    if (starts_with(s, "/=")) {
        p->tok.kind = TOK_DIV_ASSIGN;
        p->pos += 2;
        return;
    }
    if (starts_with(s, "%=")) {
        p->tok.kind = TOK_MOD_ASSIGN;
        p->pos += 2;
        return;
    }
    if (starts_with(s, "&=")) {
        p->tok.kind = TOK_AND_ASSIGN;
        p->pos += 2;
        return;
    }
    if (starts_with(s, "^=")) {
        p->tok.kind = TOK_XOR_ASSIGN;
        p->pos += 2;
        return;
    }
    if (starts_with(s, "|=")) {
        p->tok.kind = TOK_OR_ASSIGN;
        p->pos += 2;
        return;
    }

    switch (*s) {
    case '(':
        p->tok.kind = TOK_LPAREN;
        p->pos++;
        return;
    case ')':
        p->tok.kind = TOK_RPAREN;
        p->pos++;
        return;
    case '?':
        p->tok.kind = TOK_QUESTION;
        p->pos++;
        return;
    case ':':
        p->tok.kind = TOK_COLON;
        p->pos++;
        return;
    case '+':
        p->tok.kind = TOK_PLUS;
        p->pos++;
        return;
    case '-':
        p->tok.kind = TOK_MINUS;
        p->pos++;
        return;
    case '*':
        p->tok.kind = TOK_STAR;
        p->pos++;
        return;
    case '/':
        p->tok.kind = TOK_SLASH;
        p->pos++;
        return;
    case '%':
        p->tok.kind = TOK_PERCENT;
        p->pos++;
        return;
    case '<':
        p->tok.kind = TOK_LT;
        p->pos++;
        return;
    case '>':
        p->tok.kind = TOK_GT;
        p->pos++;
        return;
    case '&':
        p->tok.kind = TOK_AMP;
        p->pos++;
        return;
    case '^':
        p->tok.kind = TOK_CARET;
        p->pos++;
        return;
    case '|':
        p->tok.kind = TOK_PIPE;
        p->pos++;
        return;
    case '~':
        p->tok.kind = TOK_TILDE;
        p->pos++;
        return;
    case '!':
        p->tok.kind = TOK_BANG;
        p->pos++;
        return;
    case '=':
        p->tok.kind = TOK_ASSIGN;
        p->pos++;
        return;
    default:
        parser_fail(p, "malformed arithmetic expansion");
        p->pos++;
        p->tok.kind = TOK_END;
        return;
    }
}

static bool token_is_assignment(enum arith_token_kind kind) {
    return kind == TOK_ASSIGN || kind == TOK_MUL_ASSIGN ||
           kind == TOK_DIV_ASSIGN || kind == TOK_MOD_ASSIGN ||
           kind == TOK_ADD_ASSIGN || kind == TOK_SUB_ASSIGN ||
           kind == TOK_SHL_ASSIGN || kind == TOK_SHR_ASSIGN ||
           kind == TOK_AND_ASSIGN || kind == TOK_XOR_ASSIGN ||
           kind == TOK_OR_ASSIGN;
}

static struct arith_value make_value(long value) {
    struct arith_value v;

    v.value = value;
    v.assignable = false;
    v.name_start = 0;
    v.name_len = 0;
    return v;
}

static char *slice_dup(const char *src, size_t start, size_t len) {
    char *copy;

    copy = malloc(len + 1);
    if (copy == NULL) {
        perror("malloc");
        return NULL;
    }

    memcpy(copy, src + start, len);
    copy[len] = '\0';
    return copy;
}

static bool parse_long_text(const char *text, long *out_value) {
    char *end;
    long value;

    errno = 0;
    value = strtol(text, &end, 0);
    if (end == text || *end != '\0' || errno == ERANGE) {
        return false;
    }

    *out_value = value;
    return true;
}

static long read_identifier_value(struct arith_parser *p, size_t start,
                                  size_t len, bool *ok) {
    char *name;
    const char *value;
    long result;

    name = slice_dup(p->src, start, len);
    if (name == NULL) {
        parser_fail(p, "malloc failed");
        *ok = false;
        return 0;
    }

    value = getenv(name);
    if (value == NULL || value[0] == '\0') {
        free(name);
        *ok = true;
        return 0;
    }

    if (!parse_long_text(value, &result)) {
        /* Keep M1 behavior simple: non-numeric values are treated as zero. */
        result = 0;
    }

    free(name);
    *ok = true;
    return result;
}

static int assign_identifier_value(struct arith_parser *p, size_t start,
                                   size_t len, long value) {
    char *name;
    char text[64];

    name = slice_dup(p->src, start, len);
    if (name == NULL) {
        parser_fail(p, "malloc failed");
        return -1;
    }

    snprintf(text, sizeof(text), "%ld", value);
    if (vars_set(p->state, name, text, true) != 0) {
        if (!p->state->interactive) {
            p->state->should_exit = true;
            p->state->exit_status = 1;
        }
        free(name);
        p->failed = true;
        return -1;
    }

    free(name);
    return 0;
}

static struct arith_value parse_primary(struct arith_parser *p, bool eval) {
    struct arith_value out;

    if (p->tok.kind == TOK_NUMBER) {
        out = make_value(p->tok.number);
        next_token(p);
        return out;
    }

    if (p->tok.kind == TOK_IDENT) {
        bool ok;

        out = make_value(0);
        out.assignable = true;
        out.name_start = p->tok.start;
        out.name_len = p->tok.len;

        if (eval) {
            out.value = read_identifier_value(p, out.name_start, out.name_len, &ok);
            if (!ok) {
                return make_value(0);
            }
        }

        next_token(p);
        return out;
    }

    if (p->tok.kind == TOK_LPAREN) {
        out = make_value(0);
        next_token(p);
        out = parse_assignment(p, eval);
        out.assignable = false;
        if (p->tok.kind != TOK_RPAREN) {
            parser_fail(p, "malformed arithmetic expansion");
            return make_value(0);
        }
        next_token(p);
        return out;
    }

    parser_fail(p, "malformed arithmetic expansion");
    return make_value(0);
}

static struct arith_value parse_unary(struct arith_parser *p, bool eval) {
    enum arith_token_kind op;
    struct arith_value rhs;

    if (p->tok.kind != TOK_PLUS && p->tok.kind != TOK_MINUS &&
        p->tok.kind != TOK_BANG && p->tok.kind != TOK_TILDE) {
        return parse_primary(p, eval);
    }

    op = p->tok.kind;
    next_token(p);
    rhs = parse_unary(p, eval);
    rhs.assignable = false;

    if (!eval || p->failed) {
        return rhs;
    }

    if (op == TOK_PLUS) {
        rhs.value = +rhs.value;
    } else if (op == TOK_MINUS) {
        rhs.value = -rhs.value;
    } else if (op == TOK_BANG) {
        rhs.value = rhs.value == 0 ? 1 : 0;
    } else {
        rhs.value = ~rhs.value;
    }
    return rhs;
}

static struct arith_value parse_multiplicative(struct arith_parser *p, bool eval) {
    struct arith_value left;

    left = parse_unary(p, eval);

    while (p->tok.kind == TOK_STAR || p->tok.kind == TOK_SLASH ||
           p->tok.kind == TOK_PERCENT) {
        enum arith_token_kind op;
        struct arith_value right;

        op = p->tok.kind;
        next_token(p);
        right = parse_unary(p, eval);

        if (!eval || p->failed) {
            left.assignable = false;
            continue;
        }

        if ((op == TOK_SLASH || op == TOK_PERCENT) && right.value == 0) {
            parser_fail(p, "arithmetic error: division by zero");
            return make_value(0);
        }

        if (op == TOK_STAR) {
            left.value *= right.value;
        } else if (op == TOK_SLASH) {
            left.value /= right.value;
        } else {
            left.value %= right.value;
        }
        left.assignable = false;
    }

    return left;
}

static struct arith_value parse_additive(struct arith_parser *p, bool eval) {
    struct arith_value left;

    left = parse_multiplicative(p, eval);

    while (p->tok.kind == TOK_PLUS || p->tok.kind == TOK_MINUS) {
        enum arith_token_kind op;
        struct arith_value right;

        op = p->tok.kind;
        next_token(p);
        right = parse_multiplicative(p, eval);

        if (!eval || p->failed) {
            left.assignable = false;
            continue;
        }

        if (op == TOK_PLUS) {
            left.value += right.value;
        } else {
            left.value -= right.value;
        }
        left.assignable = false;
    }

    return left;
}

static struct arith_value parse_shift(struct arith_parser *p, bool eval) {
    struct arith_value left;

    left = parse_additive(p, eval);

    while (p->tok.kind == TOK_SHL || p->tok.kind == TOK_SHR) {
        enum arith_token_kind op;
        struct arith_value right;

        op = p->tok.kind;
        next_token(p);
        right = parse_additive(p, eval);

        if (!eval || p->failed) {
            left.assignable = false;
            continue;
        }

        if (op == TOK_SHL) {
            left.value <<= right.value;
        } else {
            left.value >>= right.value;
        }
        left.assignable = false;
    }

    return left;
}

static struct arith_value parse_relational(struct arith_parser *p, bool eval) {
    struct arith_value left;

    left = parse_shift(p, eval);

    while (p->tok.kind == TOK_LT || p->tok.kind == TOK_LE ||
           p->tok.kind == TOK_GT || p->tok.kind == TOK_GE) {
        enum arith_token_kind op;
        struct arith_value right;

        op = p->tok.kind;
        next_token(p);
        right = parse_shift(p, eval);

        if (!eval || p->failed) {
            left.assignable = false;
            continue;
        }

        if (op == TOK_LT) {
            left.value = left.value < right.value ? 1 : 0;
        } else if (op == TOK_LE) {
            left.value = left.value <= right.value ? 1 : 0;
        } else if (op == TOK_GT) {
            left.value = left.value > right.value ? 1 : 0;
        } else {
            left.value = left.value >= right.value ? 1 : 0;
        }
        left.assignable = false;
    }

    return left;
}

static struct arith_value parse_equality(struct arith_parser *p, bool eval) {
    struct arith_value left;

    left = parse_relational(p, eval);

    while (p->tok.kind == TOK_EQ || p->tok.kind == TOK_NE) {
        enum arith_token_kind op;
        struct arith_value right;

        op = p->tok.kind;
        next_token(p);
        right = parse_relational(p, eval);

        if (!eval || p->failed) {
            left.assignable = false;
            continue;
        }

        if (op == TOK_EQ) {
            left.value = left.value == right.value ? 1 : 0;
        } else {
            left.value = left.value != right.value ? 1 : 0;
        }
        left.assignable = false;
    }

    return left;
}

static struct arith_value parse_bitand(struct arith_parser *p, bool eval) {
    struct arith_value left;

    left = parse_equality(p, eval);

    while (p->tok.kind == TOK_AMP) {
        struct arith_value right;

        next_token(p);
        right = parse_equality(p, eval);
        if (eval && !p->failed) {
            left.value &= right.value;
        }
        left.assignable = false;
    }

    return left;
}

static struct arith_value parse_bitxor(struct arith_parser *p, bool eval) {
    struct arith_value left;

    left = parse_bitand(p, eval);

    while (p->tok.kind == TOK_CARET) {
        struct arith_value right;

        next_token(p);
        right = parse_bitand(p, eval);
        if (eval && !p->failed) {
            left.value ^= right.value;
        }
        left.assignable = false;
    }

    return left;
}

static struct arith_value parse_bitor(struct arith_parser *p, bool eval) {
    struct arith_value left;

    left = parse_bitxor(p, eval);

    while (p->tok.kind == TOK_PIPE) {
        struct arith_value right;

        next_token(p);
        right = parse_bitxor(p, eval);
        if (eval && !p->failed) {
            left.value |= right.value;
        }
        left.assignable = false;
    }

    return left;
}

static struct arith_value parse_logical_and(struct arith_parser *p, bool eval) {
    struct arith_value left;

    left = parse_bitor(p, eval);

    while (p->tok.kind == TOK_AND_AND) {
        struct arith_value right;
        bool right_eval;

        next_token(p);
        right_eval = eval && (left.value != 0);
        right = parse_bitor(p, right_eval);

        if (eval && !p->failed) {
            left.value = (left.value != 0 && right.value != 0) ? 1 : 0;
        }
        left.assignable = false;
    }

    return left;
}

static struct arith_value parse_logical_or(struct arith_parser *p, bool eval) {
    struct arith_value left;

    left = parse_logical_and(p, eval);

    while (p->tok.kind == TOK_OR_OR) {
        struct arith_value right;
        bool right_eval;

        next_token(p);
        right_eval = eval && (left.value == 0);
        right = parse_logical_and(p, right_eval);

        if (eval && !p->failed) {
            left.value = (left.value != 0 || right.value != 0) ? 1 : 0;
        }
        left.assignable = false;
    }

    return left;
}

static struct arith_value parse_conditional(struct arith_parser *p, bool eval) {
    struct arith_value cond;

    cond = parse_logical_or(p, eval);

    if (p->tok.kind == TOK_QUESTION) {
        struct arith_value on_true;
        struct arith_value on_false;
        bool true_eval;
        bool false_eval;

        next_token(p);
        true_eval = eval && cond.value != 0;
        false_eval = eval && cond.value == 0;

        on_true = parse_assignment(p, true_eval);
        if (p->tok.kind != TOK_COLON) {
            parser_fail(p, "malformed arithmetic expansion");
            return make_value(0);
        }
        next_token(p);
        on_false = parse_conditional(p, false_eval);

        if (eval && !p->failed) {
            cond.value = (cond.value != 0) ? on_true.value : on_false.value;
        }
        cond.assignable = false;
    }

    return cond;
}

static struct arith_value parse_assignment(struct arith_parser *p, bool eval) {
    struct arith_value left;

    left = parse_conditional(p, eval);

    if (token_is_assignment(p->tok.kind)) {
        enum arith_token_kind op;
        struct arith_value right;
        long assigned;
        long lhs;
        bool assignable;
        size_t name_start;
        size_t name_len;

        op = p->tok.kind;
        assignable = left.assignable;
        name_start = left.name_start;
        name_len = left.name_len;
        next_token(p);

        right = parse_assignment(p, eval);

        if (!eval || p->failed) {
            return make_value(0);
        }

        if (!assignable && op != TOK_ASSIGN) {
            parser_fail(p, "invalid assignment target in arithmetic expansion");
            return make_value(0);
        }
        if (!assignable && op == TOK_ASSIGN) {
            parser_fail(p, "invalid assignment target in arithmetic expansion");
            return make_value(0);
        }

        lhs = left.value;
        assigned = right.value;

        if (op == TOK_MUL_ASSIGN) {
            assigned = lhs * right.value;
        } else if (op == TOK_DIV_ASSIGN) {
            if (right.value == 0) {
                parser_fail(p, "arithmetic error: division by zero");
                return make_value(0);
            }
            assigned = lhs / right.value;
        } else if (op == TOK_MOD_ASSIGN) {
            if (right.value == 0) {
                parser_fail(p, "arithmetic error: division by zero");
                return make_value(0);
            }
            assigned = lhs % right.value;
        } else if (op == TOK_ADD_ASSIGN) {
            assigned = lhs + right.value;
        } else if (op == TOK_SUB_ASSIGN) {
            assigned = lhs - right.value;
        } else if (op == TOK_SHL_ASSIGN) {
            assigned = lhs << right.value;
        } else if (op == TOK_SHR_ASSIGN) {
            assigned = lhs >> right.value;
        } else if (op == TOK_AND_ASSIGN) {
            assigned = lhs & right.value;
        } else if (op == TOK_XOR_ASSIGN) {
            assigned = lhs ^ right.value;
        } else if (op == TOK_OR_ASSIGN) {
            assigned = lhs | right.value;
        }

        if (assign_identifier_value(p, name_start, name_len, assigned) != 0) {
            return make_value(0);
        }

        return make_value(assigned);
    }

    return left;
}

int arith_eval(const char *expr, struct shell_state *state, long *out_value) {
    struct arith_parser parser;
    struct arith_value result;

    parser.src = expr;
    parser.pos = 0;
    parser.state = state;
    parser.failed = false;
    parser.tok.kind = TOK_END;
    parser.tok.number = 0;
    parser.tok.start = 0;
    parser.tok.len = 0;

    next_token(&parser);
    result = parse_assignment(&parser, true);

    if (!parser.failed && parser.tok.kind != TOK_END) {
        parser_fail(&parser, "malformed arithmetic expansion");
    }

    if (parser.failed) {
        return -1;
    }

    *out_value = result.value;
    return 0;
}
