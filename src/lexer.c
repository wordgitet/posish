/* SPDX-License-Identifier: 0BSD */

/* posish - lexer */

#include "lexer.h"

#include "arena.h"
#include "error.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>

static void buf_push(char **buf, size_t *len, size_t *cap, char ch);
static void report_lexer_error(const char *source_name, const char *line,
                               size_t base_line, const char *pos,
                               enum posish_error_id id);

static int append_command_subst(const char **p_ptr, char **buf, size_t *len,
                                size_t *cap, const char *source_name,
                                const char *line, size_t base_line) {
    const char *p;
    int depth;
    int quote;

    p = *p_ptr;
    depth = 0;
    quote = 0;
    while (*p != '\0') {
        char ch;

        ch = *p;
        buf_push(buf, len, cap, ch);

        if (quote == 0) {
            if (ch == '\\' && p[1] != '\0') {
                p++;
                buf_push(buf, len, cap, *p);
            } else if (ch == '\'' || ch == '"') {
                quote = ch;
            } else if (ch == '(') {
                depth++;
            } else if (ch == ')') {
                depth--;
                if (depth == 0) {
                    p++;
                    *p_ptr = p;
                    return 0;
                }
            }
        } else if (quote == '\'' && ch == '\'') {
            quote = 0;
        } else if (quote == '"') {
            if (ch == '\\' && p[1] != '\0') {
                p++;
                buf_push(buf, len, cap, *p);
            } else if (ch == '"') {
                quote = 0;
            }
        }

        p++;
    }

    report_lexer_error(source_name, line, base_line, p,
                       POSERR_UNTERMINATED_COMMAND_SUBSTITUTION);
    return -1;
}

static int append_backtick_subst(const char **p_ptr, char **buf, size_t *len,
                                 size_t *cap, const char *source_name,
                                 const char *line, size_t base_line) {
    const char *p;

    p = *p_ptr;
    buf_push(buf, len, cap, *p++);
    while (*p != '\0') {
        buf_push(buf, len, cap, *p);
        if (*p == '\\' && p[1] != '\0') {
            p++;
            buf_push(buf, len, cap, *p);
        } else if (*p == '`') {
            p++;
            *p_ptr = p;
            return 0;
        }
        p++;
    }

    report_lexer_error(source_name, line, base_line, p,
                       POSERR_UNTERMINATED_BACKTICK_SUBSTITUTION);
    return -1;
}

static int append_dollar_single_quote(const char **p_ptr, char **buf,
                                      size_t *len, size_t *cap,
                                      const char *source_name,
                                      const char *line, size_t base_line) {
    const char *p;

    p = *p_ptr;
    buf_push(buf, len, cap, *p++);
    buf_push(buf, len, cap, *p++);
    while (*p != '\0') {
        char ch;

        ch = *p;
        buf_push(buf, len, cap, ch);
        if (ch == '\\' && p[1] != '\0') {
            p++;
            buf_push(buf, len, cap, *p);
            p++;
            continue;
        }
        if (ch == '\'') {
            p++;
            *p_ptr = p;
            return 0;
        }
        p++;
    }

    report_lexer_error(source_name, line, base_line, p,
                       POSERR_UNTERMINATED_DOLLAR_SINGLE_QUOTE);
    return -1;
}

static int append_braced_param_subst(const char **p_ptr, char **buf, size_t *len,
                                     size_t *cap, bool dquote_context,
                                     const char *source_name, const char *line,
                                     size_t base_line) {
    const char *p;
    int depth;
    int quote;

    p = *p_ptr;
    depth = 0;
    quote = 0;
    while (*p != '\0') {
        char ch;

        ch = *p;
        buf_push(buf, len, cap, ch);
        if (quote == 0) {
            if (ch == '\\' && p[1] != '\0') {
                p++;
                buf_push(buf, len, cap, *p);
            } else if (ch == '"' || (!dquote_context && ch == '\'')) {
                quote = ch;
            } else if (ch == '{') {
                depth++;
            } else if (ch == '}') {
                depth--;
                if (depth == 0) {
                    p++;
                    *p_ptr = p;
                    return 0;
                }
            }
        } else if (quote == '\'' && ch == '\'') {
            quote = 0;
        } else if (quote == '"') {
            if (ch == '\\' && p[1] != '\0') {
                p++;
                buf_push(buf, len, cap, *p);
            } else if (ch == '"') {
                quote = 0;
            }
        }

        p++;
    }

    report_lexer_error(source_name, line, base_line, p,
                       POSERR_UNTERMINATED_PARAMETER_EXPANSION);
    return -1;
}

static void report_lexer_error(const char *source_name, const char *line,
                               size_t base_line, const char *pos,
                               enum posish_error_id id) {
    size_t line_no;
    const char *line_start;
    const char *p;

    if (source_name == NULL || line == NULL || pos == NULL) {
        posish_error_idf(id);
        return;
    }

    line_no = base_line == 0 ? 1 : base_line;
    line_start = line;
    for (p = line; *p != '\0' && p < pos; p++) {
        if (*p == '\n') {
            line_no++;
            line_start = p + 1;
        }
    }

    posish_error_at_idf(source_name, line_no, (size_t)(pos - line_start) + 1,
                        id);
}

static void vec_push(struct token_vec *vec, char *word) {
    vec->items = arena_xrealloc(vec->items, sizeof(*vec->items) * (vec->len + 1));
    vec->items[vec->len++] = word;
}

static void buf_push(char **buf, size_t *len, size_t *cap, char ch) {
    if (*len + 1 >= *cap) {
        if (*cap == 0) {
            *cap = 16;
        } else {
            *cap *= 2;
        }
        *buf = arena_xrealloc(*buf, *cap);
    }

    (*buf)[(*len)++] = ch;
}

void lexer_free_tokens(struct token_vec *tokens) {
    if (tokens == NULL) {
        return;
    }

    /*
     * Token vectors are arena-backed and reclaimed by surrounding arena
     * rewind/reset in the caller's execution scope.
     */
    tokens->items = NULL;
    tokens->len = 0;
}

int lexer_split_words_at(const char *source_name, const char *line,
                         size_t base_line, struct token_vec *out) {
    const char *p;

    out->items = NULL;
    out->len = 0;
    p = line;

    while (*p != '\0') {
        char *buf;
        size_t len;
        size_t cap;
        bool started;
        int quote;

        while (*p != '\0' && isspace((unsigned char)*p)) {
            p++;
        }
        if (*p == '\0') {
            break;
        }

        buf = NULL;
        len = 0;
        cap = 0;
        started = false;
        quote = 0;

        while (*p != '\0') {
            char ch;

            ch = *p;
            if (quote == 0 && isspace((unsigned char)ch)) {
                break;
            }

            if (quote != '\'' && ch == '$' && p[1] == '(') {
                if (append_command_subst(&p, &buf, &len, &cap, source_name,
                                         line, base_line) != 0) {
                    lexer_free_tokens(out);
                    return -1;
                }
                started = true;
                continue;
            }
            if (quote != '\'' && ch == '$' && p[1] == '\'') {
                if (append_dollar_single_quote(&p, &buf, &len, &cap,
                                               source_name, line,
                                               base_line) != 0) {
                    lexer_free_tokens(out);
                    return -1;
                }
                started = true;
                continue;
            }
            if (quote != '\'' && ch == '$' && p[1] == '{') {
                if (append_braced_param_subst(&p, &buf, &len, &cap,
                                              quote == '"', source_name, line,
                                              base_line) != 0) {
                    lexer_free_tokens(out);
                    return -1;
                }
                started = true;
                continue;
            }
            if (quote != '\'' && ch == '`') {
                if (append_backtick_subst(&p, &buf, &len, &cap, source_name,
                                          line, base_line) != 0) {
                    lexer_free_tokens(out);
                    return -1;
                }
                started = true;
                continue;
            }

            if (quote == 0 && ch == '\'') {
                buf_push(&buf, &len, &cap, ch);
                quote = '\'';
                started = true;
                p++;
                continue;
            }
            if (quote == '\'' && ch == '\'') {
                buf_push(&buf, &len, &cap, ch);
                quote = 0;
                p++;
                continue;
            }
            if (quote == 0 && ch == '"') {
                buf_push(&buf, &len, &cap, ch);
                quote = '"';
                started = true;
                p++;
                continue;
            }
            if (quote == '"' && ch == '"') {
                buf_push(&buf, &len, &cap, ch);
                quote = 0;
                p++;
                continue;
            }
            if (ch == '\\' && quote != '\'') {
                if (p[1] == '\0') {
                    report_lexer_error(source_name, line, base_line, p,
                                       POSERR_TRAILING_BACKSLASH);
                    lexer_free_tokens(out);
                    return -1;
                }
                /*
                 * Keep backslashes so expansion can still distinguish escaped
                 * quotes from quote delimiters.
                 */
                buf_push(&buf, &len, &cap, ch);
                buf_push(&buf, &len, &cap, p[1]);
                started = true;
                p += 2;
                continue;
            }

            buf_push(&buf, &len, &cap, ch);
            started = true;
            p++;
        }

        if (quote != 0) {
            report_lexer_error(source_name, line, base_line, p,
                               POSERR_UNTERMINATED_QUOTE);
            lexer_free_tokens(out);
            return -1;
        }

        if (!started) {
            continue;
        }

        buf_push(&buf, &len, &cap, '\0');
        vec_push(out, buf);
    }

    return 0;
}

int lexer_split_words(const char *line, struct token_vec *out) {
    return lexer_split_words_at(NULL, line, 1, out);
}
