/* SPDX-License-Identifier: 0BSD */

/* posish - parser entrypoints */

#include "parser.h"

#include "arena.h"
#include "compound_parse.h"
#include "error.h"
#include "lexer.h"
#include "redir.h"
#include "shell.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

struct parser_ctx {
    const char *source_name;
    size_t base_line;
    const char *source;
    size_t len;
};

static struct ast_node *parse_sequence(struct parser_ctx *ctx, size_t start,
                                       size_t end, int *err_out);
static struct ast_node *parse_andor(struct parser_ctx *ctx, size_t start,
                                    size_t end, int *err_out);
static struct ast_node *parse_pipeline(struct parser_ctx *ctx, size_t start,
                                       size_t end, int *err_out);
static struct ast_node *parse_command_atom(struct parser_ctx *ctx, size_t start,
                                           size_t end, int *err_out);
static int parse_simple_parts(struct parser_ctx *ctx, const char *source,
                              struct ast_word_vec *raw_words,
                              struct redir_vec *redirs);
static bool source_contains_heredoc_operator(const char *source);
static bool parse_function_definition_text(const char *source, char **name_out,
                                           char **body_out);
static size_t line_at_offset(const struct parser_ctx *ctx, size_t offset);
static struct ast_node *parse_embedded_program_root(struct parser_ctx *ctx,
                                                    const char *source,
                                                    size_t offset_hint,
                                                    int *err_out);
static bool parse_case_structure(struct parser_ctx *ctx, const char *source,
                                 size_t source_offset, char **word_expr_out,
                                 struct ast_case_clause **clauses_out,
                                 size_t *clause_count_out, int *err_out);
static char *dup_trimmed_slice(const char *src, size_t start, size_t end);

static bool keyword_boundary(char ch) {
    return ch == '\0' || isspace((unsigned char)ch) || ch == ';' ||
           ch == '&' || ch == '|' || ch == '(' || ch == ')' || ch == '{' ||
           ch == '}';
}

static bool word_starts_command_position(const char *source, size_t pos) {
    size_t i;

    if (pos == 0) {
        return true;
    }

    i = pos;
    while (i > 0) {
        char ch;

        ch = source[i - 1];
        if (ch == ' ' || ch == '\t') {
            i--;
            continue;
        }
        if (ch == '\n' || ch == ';' || ch == '&' || ch == '|' || ch == '(' ||
            ch == ')' || ch == '{' || ch == '}') {
            return true;
        }
        break;
    }

    if (i == 0) {
        return true;
    }

    if (isalnum((unsigned char)source[i - 1]) || source[i - 1] == '_') {
        size_t start;
        size_t len;

        start = i - 1;
        while (start > 0 &&
               (isalnum((unsigned char)source[start - 1]) ||
                source[start - 1] == '_')) {
            start--;
        }
        len = i - start;
        if ((len == 4 && strncmp(source + start, "then", 4) == 0) ||
            (len == 2 && strncmp(source + start, "do", 2) == 0) ||
            (len == 4 && strncmp(source + start, "else", 4) == 0) ||
            (len == 4 && strncmp(source + start, "elif", 4) == 0) ||
            (len == 2 && strncmp(source + start, "if", 2) == 0) ||
            (len == 2 && strncmp(source + start, "fi", 2) == 0)) {
            return true;
        }
    }

    return false;
}

static bool keyword_preceded_by_list_separator(const char *source, size_t pos) {
    size_t i;
    char ch;

    i = pos;
    while (i > 0 && (source[i - 1] == ' ' || source[i - 1] == '\t')) {
        i--;
    }
    if (i == 0) {
        return true;
    }

    ch = source[i - 1];
    return ch == '\n' || ch == ';' || ch == '&' || ch == '|' || ch == '(' ||
           ch == ')' || ch == '{' || ch == '}';
}

static bool newline_continues_command(const char *source, size_t pos) {
    size_t i;

    if (source[pos] != '\n') {
        return false;
    }

    i = pos;
    while (i > 0) {
        char ch;

        ch = source[i - 1];
        if (ch == ' ' || ch == '\t' || ch == '\n') {
            i--;
            continue;
        }
        break;
    }
    if (i == 0) {
        return false;
    }

    if (source[i - 1] == '|') {
        return true;
    }
    if (source[i - 1] == '&' && i >= 2 && source[i - 2] == '&') {
        return true;
    }
    return false;
}

static size_t skip_continuations_forward(const char *source, size_t pos) {
    while (source[pos] == '\\' && source[pos + 1] == '\n') {
        pos += 2;
    }
    return pos;
}

static long previous_logical_index(const char *source, size_t pos) {
    long i;

    i = (long)pos - 1;
    while (i >= 1 && source[i - 1] == '\\' && source[i] == '\n') {
        i -= 2;
    }
    return i;
}

static bool is_async_separator_amp(const char *source, size_t pos) {
    size_t next;
    long prev;

    if (source[pos] != '&') {
        return false;
    }

    next = skip_continuations_forward(source, pos + 1);
    if (source[next] == '&') {
        return false;
    }

    prev = previous_logical_index(source, pos);
    if (prev >= 0 &&
        (source[prev] == '&' || source[prev] == '<' || source[prev] == '>')) {
        return false;
    }
    return true;
}

static bool find_command_subst_end(const char *source, size_t start,
                                   size_t *end_out) {
    size_t i;
    int depth;
    char quote;

    if (source[start] != '$' || source[start + 1] != '(') {
        return false;
    }

    i = start + 2;
    depth = 1;
    quote = '\0';
    while (source[i] != '\0') {
        char ch;

        ch = source[i];
        if (quote == '\0') {
            if (ch == '\\' && source[i + 1] != '\0') {
                i += 2;
                continue;
            }
            if (ch == '\'' || ch == '"') {
                quote = ch;
                i++;
                continue;
            }
            if (ch == '(') {
                depth++;
                i++;
                continue;
            }
            if (ch == ')') {
                depth--;
                if (depth == 0) {
                    *end_out = i;
                    return true;
                }
            }
            i++;
            continue;
        }

        if (quote == '\'' && ch == '\'') {
            quote = '\0';
            i++;
            continue;
        }
        if (quote == '"') {
            if (ch == '\\' && source[i + 1] != '\0') {
                i += 2;
                continue;
            }
            if (ch == '"') {
                quote = '\0';
            }
        }
        i++;
    }

    return false;
}

static bool find_dollar_single_quote_end(const char *source, size_t start,
                                         size_t *end_out) {
    size_t i;

    if (source[start] != '$' || source[start + 1] != '\'') {
        return false;
    }

    i = start + 2;
    while (source[i] != '\0') {
        if (source[i] == '\\' && source[i + 1] != '\0') {
            i += 2;
            continue;
        }
        if (source[i] == '\'') {
            *end_out = i;
            return true;
        }
        i++;
    }
    return false;
}

static bool is_name_start_char(char ch) {
    return isalpha((unsigned char)ch) || ch == '_';
}

static bool is_name_char(char ch) {
    return isalnum((unsigned char)ch) || ch == '_';
}

static bool parse_function_definition_text(const char *source, char **name_out,
                                           char **body_out) {
    size_t i;
    size_t name_start;
    size_t name_end;
    size_t body_start;
    size_t body_end;

    *name_out = NULL;
    *body_out = NULL;

    i = 0;
    while (isspace((unsigned char)source[i])) {
        i++;
    }
    name_start = i;
    if (!is_name_start_char(source[i])) {
        return false;
    }
    i++;
    while (is_name_char(source[i])) {
        i++;
    }
    name_end = i;

    while (isspace((unsigned char)source[i])) {
        i++;
    }
    if (source[i] != '(') {
        return false;
    }
    i++;
    while (isspace((unsigned char)source[i])) {
        i++;
    }
    if (source[i] != ')') {
        return false;
    }
    i++;
    while (isspace((unsigned char)source[i])) {
        i++;
    }
    if (source[i] == '\0') {
        return false;
    }

    body_start = i;
    body_end = strlen(source);
    while (body_end > body_start &&
           isspace((unsigned char)source[body_end - 1])) {
        body_end--;
    }
    if (body_end <= body_start) {
        return false;
    }

    *name_out = dup_trimmed_slice(source, name_start, name_end);
    *body_out = dup_trimmed_slice(source, body_start, body_end);
    return true;
}

static char *dup_slice(const char *src, size_t start, size_t end) {
    char *out;
    size_t len;

    len = end - start;
    out = arena_xmalloc(len + 1);
    if (len > 0) {
        memcpy(out, src + start, len);
    }
    out[len] = '\0';
    return out;
}

static char *dup_trimmed_slice(const char *src, size_t start, size_t end) {
    while (start < end && isspace((unsigned char)src[start])) {
        start++;
    }
    while (end > start && isspace((unsigned char)src[end - 1])) {
        end--;
    }
    return dup_slice(src, start, end);
}

static void span_from_offsets(struct parser_ctx *ctx, size_t start, size_t end,
                              struct ast_span *span) {
    size_t i;
    size_t line;
    size_t col;

    line = ctx->base_line == 0 ? 1 : ctx->base_line;
    col = 1;
    memset(span, 0, sizeof(*span));
    span->start_offset = start;
    span->end_offset = end;

    for (i = 0; i < ctx->len && i < end; i++) {
        if (i == start) {
            span->start_line = line;
            span->start_col = col;
        }
        if (ctx->source[i] == '\n') {
            line++;
            col = 1;
        } else {
            col++;
        }
    }

    if (start >= end) {
        span->start_line = line;
        span->start_col = col;
    }
    span->end_line = line;
    span->end_col = col;
}

static size_t line_at_offset(const struct parser_ctx *ctx, size_t offset) {
    size_t i;
    size_t line;

    line = ctx->base_line == 0 ? 1 : ctx->base_line;
    for (i = 0; i < ctx->len && i < offset; i++) {
        if (ctx->source[i] == '\n') {
            line++;
        }
    }
    return line;
}

static struct ast_node *parse_embedded_program_root(struct parser_ctx *ctx,
                                                    const char *source,
                                                    size_t offset_hint,
                                                    int *err_out) {
    struct ast_program *program;

    program = NULL;
    if (parse_program_at(ctx->source_name, line_at_offset(ctx, offset_hint),
                         source, &program) != 0) {
        *err_out = -1;
        return NULL;
    }
    return program->root;
}

static struct ast_node *ast_new_node(struct parser_ctx *ctx,
                                     enum ast_node_kind kind,
                                     size_t start,
                                     size_t end) {
    struct ast_node *node;

    node = arena_xmalloc(sizeof(*node));
    memset(node, 0, sizeof(*node));
    node->kind = kind;
    span_from_offsets(ctx, start, end, &node->span);
    node->source = dup_trimmed_slice(ctx->source, start, end);
    return node;
}

static void ast_node_vec_push(struct ast_node ***items, size_t *len,
                              struct ast_node *node) {
    *items = arena_xrealloc(*items, sizeof(**items) * (*len + 1));
    (*items)[(*len)++] = node;
}

static void ast_andor_op_push(enum ast_andor_op **items, size_t *len,
                              enum ast_andor_op op) {
    *items = arena_xrealloc(*items, sizeof(**items) * (*len + 1));
    (*items)[(*len)++] = op;
}

static void ast_word_vec_push(struct ast_word_vec *words, char *word) {
    words->items =
        arena_xrealloc(words->items, sizeof(*words->items) * (words->len + 1));
    words->items[words->len++] = word;
}

static int find_redir_operator_pos(const char *token, size_t *pos_out) {
    size_t i;
    char quote;

    quote = '\0';
    for (i = 0; token[i] != '\0'; i++) {
        char ch;

        ch = token[i];
        if (quote == '\0') {
            if (ch == '$' && token[i + 1] == '(') {
                size_t end;

                if (find_command_subst_end(token, i, &end)) {
                    i = end;
                    continue;
                }
            }
            if (ch == '\\' && token[i + 1] != '\0') {
                i++;
                continue;
            }
            if (ch == '\'' || ch == '"') {
                quote = ch;
                continue;
            }
            if (ch == '<' || ch == '>') {
                *pos_out = i;
                return 0;
            }
            continue;
        }

        if (quote == '\'' && ch == '\'') {
            quote = '\0';
        } else if (quote == '"') {
            if (ch == '\\' && token[i + 1] != '\0') {
                i++;
                continue;
            }
            if (ch == '"') {
                quote = '\0';
            }
        }
    }

    return -1;
}

static int collect_words_and_redirs(const struct token_vec *lexed,
                                    struct ast_word_vec *words,
                                    struct redir_vec *redirs) {
    size_t i;

    words->items = NULL;
    words->len = 0;
    redirs->items = NULL;
    redirs->len = 0;

    for (i = 0; i < lexed->len; i++) {
        struct redir_spec spec;
        bool needs_word;
        int pr;

        pr = parse_redir_token(lexed->items[i], &spec, &needs_word);
        if (pr < 0) {
            return -1;
        }
        if (pr == 0) {
            size_t op_pos;

            if (find_redir_operator_pos(lexed->items[i], &op_pos) == 0 &&
                op_pos > 0) {
                char *prefix;
                const char *redir_text;

                prefix = arena_xmalloc(op_pos + 1);
                memcpy(prefix, lexed->items[i], op_pos);
                prefix[op_pos] = '\0';
                ast_word_vec_push(words, prefix);

                redir_text = lexed->items[i] + op_pos;
                pr = parse_redir_token(redir_text, &spec, &needs_word);
                if (pr < 0) {
                    return -1;
                }
                if (pr == 0) {
                    ast_word_vec_push(words, lexed->items[i]);
                    continue;
                }
            } else {
                ast_word_vec_push(words, lexed->items[i]);
                continue;
            }
        }

        if (needs_word) {
            i++;
            if (i >= lexed->len) {
                posish_error_idf(POSERR_MISSING_REDIRECTION_OPERAND);
                return -1;
            }
            spec.path = arena_xstrdup(lexed->items[i]);
        }

        redir_vec_push(redirs, &spec);
    }

    return 0;
}

static void report_unexpected_token(struct parser_ctx *ctx, size_t pos,
                                    const char *token) {
    size_t line;
    size_t col;
    size_t i;

    line = ctx->base_line == 0 ? 1 : ctx->base_line;
    col = 1;
    for (i = 0; i < ctx->len && i < pos; i++) {
        if (ctx->source[i] == '\n') {
            line++;
            col = 1;
        } else {
            col++;
        }
    }

    posish_error_at_idf(ctx->source_name, line, col, POSERR_UNEXPECTED_TOKEN,
                        token);
}

static bool unwrap_subshell_group(const char *source, size_t *inner_start_out,
                                  size_t *inner_end_out,
                                  size_t *redir_start_out) {
    size_t len;
    size_t i;
    int paren_depth;
    char quote;
    size_t close_pos;

    len = strlen(source);
    if (len < 2 || source[0] != '(') {
        return false;
    }

    quote = '\0';
    paren_depth = 0;
    close_pos = (size_t)-1;
    for (i = 0; i < len; i++) {
        char ch;

        ch = source[i];
        if (quote == '\0') {
            if (ch == '\\' && source[i + 1] != '\0') {
                i++;
                continue;
            }
            if (ch == '\'' || ch == '"') {
                quote = ch;
                continue;
            }
            if (ch == '(') {
                paren_depth++;
            } else if (ch == ')') {
                paren_depth--;
                if (paren_depth < 0) {
                    return false;
                }
                if (paren_depth == 0) {
                    close_pos = i;
                    break;
                }
            }
        } else if (quote == '\'' && ch == '\'') {
            quote = '\0';
        } else if (quote == '"') {
            if (ch == '\\' && source[i + 1] != '\0') {
                i++;
                continue;
            }
            if (ch == '"') {
                quote = '\0';
            }
        }
    }

    if (quote != '\0' || paren_depth != 0 || close_pos == (size_t)-1) {
        return false;
    }

    *inner_start_out = 1;
    *inner_end_out = close_pos;
    *redir_start_out = close_pos + 1;
    return true;
}

static bool unwrap_brace_group(const char *source, size_t *inner_start_out,
                               size_t *inner_end_out,
                               size_t *redir_start_out) {
    size_t len;
    size_t i;
    int brace_depth;
    char quote;
    size_t close_pos;

    len = strlen(source);
    if (len < 2 || source[0] != '{') {
        return false;
    }

    quote = '\0';
    brace_depth = 0;
    close_pos = (size_t)-1;
    for (i = 0; i < len; i++) {
        char ch;

        ch = source[i];
        if (quote == '\0') {
            if (ch == '\\' && source[i + 1] != '\0') {
                i++;
                continue;
            }
            if (ch == '\'' || ch == '"') {
                quote = ch;
                continue;
            }

            if (ch == '{') {
                brace_depth++;
            } else if (ch == '}') {
                brace_depth--;
                if (brace_depth == 0) {
                    close_pos = i;
                    break;
                }
                if (brace_depth < 0) {
                    return false;
                }
            }
        } else if (quote == '\'' && ch == '\'') {
            quote = '\0';
        } else if (quote == '"') {
            if (ch == '\\' && source[i + 1] != '\0') {
                i++;
                continue;
            }
            if (ch == '"') {
                quote = '\0';
            }
        }
    }

    if (quote != '\0' || brace_depth != 0 || close_pos == (size_t)-1) {
        return false;
    }

    *inner_start_out = 1;
    *inner_end_out = close_pos;
    *redir_start_out = close_pos + 1;
    return true;
}

static bool split_case_redirection_suffix(const char *source, char **core_out,
                                          char **suffix_out) {
    size_t i;
    int case_depth;
    int paren_depth;
    int brace_depth;
    char quote;
    size_t end_pos;

    *core_out = NULL;
    *suffix_out = NULL;

    i = 0;
    while (isspace((unsigned char)source[i])) {
        i++;
    }
    if (strncmp(source + i, "case", 4) != 0 || !keyword_boundary(source[i + 4])) {
        return false;
    }

    case_depth = 0;
    paren_depth = 0;
    brace_depth = 0;
    quote = '\0';
    end_pos = 0;

    for (i = 0; source[i] != '\0'; i++) {
        char ch;

        ch = source[i];
        if (quote == '\0') {
            if (ch == '\\' && source[i + 1] != '\0') {
                i++;
                continue;
            }
            if (ch == '\'' || ch == '"') {
                quote = ch;
                continue;
            }
            if (paren_depth == 0 && brace_depth == 0 &&
                (isalpha((unsigned char)ch) || ch == '_') &&
                word_starts_command_position(source, i)) {
                size_t j;
                size_t boundary;
                char keyword[16];
                size_t kwlen;

                j = i;
                kwlen = 0;
                while (source[j] != '\0') {
                    if (source[j] == '\\' && source[j + 1] == '\n') {
                        j += 2;
                        continue;
                    }
                    if (!isalnum((unsigned char)source[j]) && source[j] != '_') {
                        break;
                    }
                    if (kwlen + 1 < sizeof(keyword)) {
                        keyword[kwlen] = source[j];
                    }
                    kwlen++;
                    j++;
                }
                boundary = skip_continuations_forward(source, j);
                if (keyword_boundary(source[boundary]) && source[boundary] != ')') {
                    if (kwlen == 4 && strncmp(keyword, "case", 4) == 0) {
                        case_depth++;
                    } else if (kwlen == 4 && strncmp(keyword, "esac", 4) == 0 &&
                               case_depth > 0) {
                        case_depth--;
                        if (case_depth == 0) {
                            end_pos = boundary;
                            break;
                        }
                    }
                }
                i = j - 1;
                continue;
            }
            if (ch == '(') {
                paren_depth++;
                continue;
            }
            if (ch == ')' && paren_depth > 0) {
                paren_depth--;
                continue;
            }
            if (ch == '{') {
                brace_depth++;
                continue;
            }
            if (ch == '}' && brace_depth > 0) {
                brace_depth--;
                continue;
            }
        } else if (quote == '\'' && ch == '\'') {
            quote = '\0';
        } else if (quote == '"') {
            if (ch == '\\' && source[i + 1] != '\0') {
                i++;
                continue;
            }
            if (ch == '"') {
                quote = '\0';
            }
        }
    }

    if (case_depth != 0 || end_pos == 0) {
        return false;
    }

    *core_out = dup_trimmed_slice(source, 0, end_pos);
    *suffix_out = dup_trimmed_slice(source, end_pos, strlen(source));
    return true;
}

static int parse_simple_parts(struct parser_ctx *ctx, const char *source,
                              struct ast_word_vec *raw_words,
                              struct redir_vec *redirs) {
    struct token_vec lexed;

    lexed.items = NULL;
    lexed.len = 0;

    if (lexer_split_words_at(ctx->source_name, source, ctx->base_line, &lexed) != 0) {
        return -1;
    }
    if (collect_words_and_redirs(&lexed, raw_words, redirs) != 0) {
        lexer_free_tokens(&lexed);
        return -1;
    }
    lexer_free_tokens(&lexed);
    return 0;
}

static bool source_contains_heredoc_operator(const char *source) {
    size_t i;
    char quote;

    quote = '\0';
    for (i = 0; source[i] != '\0'; i++) {
        char ch;

        ch = source[i];
        if (quote == '\0') {
            if (ch == '\\' && source[i + 1] != '\0') {
                i++;
                continue;
            }
            if (ch == '\'' || ch == '"') {
                quote = ch;
                continue;
            }
            if (ch == '<' && source[i + 1] == '<') {
                return true;
            }
            continue;
        }

        if (quote == '\'' && ch == '\'') {
            quote = '\0';
        } else if (quote == '"') {
            if (ch == '\\' && source[i + 1] != '\0') {
                i++;
                continue;
            }
            if (ch == '"') {
                quote = '\0';
            }
        }
    }

    return false;
}

static void ast_case_clause_vec_push(struct ast_case_clause **items,
                                     size_t *len,
                                     const struct ast_case_clause *item) {
    *items = arena_xrealloc(*items, sizeof(**items) * (*len + 1));
    (*items)[*len] = *item;
    (*len)++;
}

static size_t skip_case_clause_leading_space_and_comments_parser(
    const char *source, size_t pos) {
    while (1) {
        while (isspace((unsigned char)source[pos])) {
            pos++;
        }
        if (source[pos] != '#') {
            return pos;
        }
        while (source[pos] != '\0' && source[pos] != '\n') {
            pos++;
        }
    }
}

static bool skip_backtick_subst_parser(const char *source, size_t start,
                                       size_t *out_end) {
    size_t i;

    i = start + 1;
    while (source[i] != '\0') {
        if (source[i] == '\\' && source[i + 1] != '\0') {
            i += 2;
            continue;
        }
        if (source[i] == '`') {
            *out_end = i;
            return true;
        }
        i++;
    }
    return false;
}

static bool skip_braced_param_parser(const char *source, size_t start,
                                     size_t *out_end) {
    size_t i;
    int depth;
    char quote;

    i = start + 2;
    depth = 1;
    quote = '\0';
    while (source[i] != '\0') {
        char ch;

        ch = source[i];
        if (quote == '\0') {
            if (ch == '\\' && source[i + 1] != '\0') {
                i += 2;
                continue;
            }
            if (ch == '\'' || ch == '"') {
                quote = ch;
                i++;
                continue;
            }
            if (ch == '{') {
                depth++;
            } else if (ch == '}') {
                depth--;
                if (depth == 0) {
                    *out_end = i;
                    return true;
                }
            }
        } else if ((quote == '\'' && ch == '\'') ||
                   (quote == '"' && ch == '"')) {
            quote = '\0';
        } else if (quote == '"' && ch == '\\' && source[i + 1] != '\0') {
            i++;
        }
        i++;
    }
    return false;
}

static bool find_pattern_clause_close_parser(const char *source, size_t start,
                                             size_t *out_end) {
    size_t i;
    char quote;

    i = start;
    quote = '\0';
    while (source[i] != '\0') {
        char ch;

        ch = source[i];
        if (quote == '\0') {
            if (ch == '\\' && source[i + 1] != '\0') {
                i += 2;
                continue;
            }
            if (ch == '\'' || ch == '"') {
                quote = ch;
                i++;
                continue;
            }
            if (ch == '`') {
                size_t end;

                if (!skip_backtick_subst_parser(source, i, &end)) {
                    return false;
                }
                i = end + 1;
                continue;
            }
            if (ch == '$' && source[i + 1] == '{') {
                size_t end;

                if (!skip_braced_param_parser(source, i, &end)) {
                    return false;
                }
                i = end + 1;
                continue;
            }
            if (ch == '$' && source[i + 1] == '(') {
                size_t end;

                if (!find_command_subst_end(source, i, &end)) {
                    return false;
                }
                i = end + 1;
                continue;
            }
            if (ch == ')') {
                *out_end = i;
                return true;
            }
            i++;
            continue;
        }

        if ((quote == '\'' && ch == '\'') || (quote == '"' && ch == '"')) {
            quote = '\0';
        } else if (quote == '"' && ch == '\\' && source[i + 1] != '\0') {
            i++;
        }
        i++;
    }

    return false;
}

static bool parse_case_structure(struct parser_ctx *ctx, const char *source,
                                 size_t source_offset, char **word_expr_out,
                                 struct ast_case_clause **clauses_out,
                                 size_t *clause_count_out, int *err_out) {
    size_t i;
    size_t word_start;
    size_t word_end;
    bool found_in;
    char quote;

    *word_expr_out = NULL;
    *clauses_out = NULL;
    *clause_count_out = 0;

    i = 0;
    while (isspace((unsigned char)source[i])) {
        i++;
    }
    if (strncmp(source + i, "case", 4) != 0 ||
        !keyword_boundary(source[i + 4])) {
        return false;
    }
    i += 4;

    while (isspace((unsigned char)source[i])) {
        i++;
    }
    word_start = i;
    word_end = i;
    found_in = false;
    quote = '\0';

    for (; source[i] != '\0'; i++) {
        char ch;

        ch = source[i];
        if (quote == '\0') {
            if (ch == '\\' && source[i + 1] != '\0') {
                i++;
                continue;
            }
            if (ch == '\'' || ch == '"') {
                quote = ch;
                continue;
            }
            if (isspace((unsigned char)ch)) {
                size_t j;

                j = i;
                while (isspace((unsigned char)source[j])) {
                    j++;
                }
                if (strncmp(source + j, "in", 2) == 0 &&
                    keyword_boundary(source[j + 2])) {
                    word_end = i;
                    i = j + 2;
                    found_in = true;
                    break;
                }
            }
        } else if (quote == '\'' && ch == '\'') {
            quote = '\0';
        } else if (quote == '"') {
            if (ch == '\\' && source[i + 1] != '\0') {
                i++;
                continue;
            }
            if (ch == '"') {
                quote = '\0';
            }
        }
    }

    if (!found_in) {
        return false;
    }

    *word_expr_out = dup_trimmed_slice(source, word_start, word_end);

    while (1) {
        size_t pat_start;
        size_t pat_end;
        size_t cmd_start;
        size_t cmd_end;
        bool clause_ended_with_esac;
        enum ast_case_clause_term terminator;
        struct ast_case_clause clause;

        while (isspace((unsigned char)source[i])) {
            i++;
        }

        if (strncmp(source + i, "esac", 4) == 0 &&
            keyword_boundary(source[i + 4])) {
            size_t j;

            j = i + 4;
            while (isspace((unsigned char)source[j])) {
                j++;
            }
            if (source[j] != '\0') {
                return false;
            }
            break;
        }

        if (source[i] == '(') {
            i++;
        }
        pat_start = i;
        if (!find_pattern_clause_close_parser(source, i, &pat_end)) {
            return false;
        }
        i = pat_end + 1;

        i = skip_case_clause_leading_space_and_comments_parser(source, i);
        cmd_start = i;
        quote = '\0';
        clause_ended_with_esac = false;
        terminator = AST_CASE_TERM_END;
        {
            int nested_case_depth;

            nested_case_depth = 0;
            for (; source[i] != '\0'; i++) {
                char ch;

                ch = source[i];
                if (quote == '\0') {
                    if (ch == '\\' && source[i + 1] != '\0') {
                        i++;
                        continue;
                    }
                    if (ch == '\'' || ch == '"') {
                        quote = ch;
                        continue;
                    }
                    if ((isalpha((unsigned char)ch) || ch == '_') &&
                        word_starts_command_position(source, i)) {
                        if (strncmp(source + i, "case", 4) == 0 &&
                            keyword_boundary(source[i + 4])) {
                            nested_case_depth++;
                            i += 3;
                            continue;
                        }
                        if (strncmp(source + i, "esac", 4) == 0 &&
                            keyword_boundary(source[i + 4])) {
                            if (nested_case_depth == 0) {
                                clause_ended_with_esac = true;
                                break;
                            }
                            nested_case_depth--;
                            i += 3;
                            continue;
                        }
                    }
                    if (nested_case_depth == 0 && ch == ';' &&
                        source[i + 1] == ';' && source[i + 2] == '&') {
                        terminator = AST_CASE_TERM_DBL_SEMI_AMP;
                        break;
                    }
                    if (nested_case_depth == 0 && ch == ';' &&
                        source[i + 1] == ';') {
                        terminator = AST_CASE_TERM_DBL_SEMI;
                        break;
                    }
                    if (nested_case_depth == 0 && ch == ';' &&
                        source[i + 1] == '&') {
                        terminator = AST_CASE_TERM_SEMI_AMP;
                        break;
                    }
                } else if (quote == '\'' && ch == '\'') {
                    quote = '\0';
                } else if (quote == '"') {
                    if (ch == '\\' && source[i + 1] != '\0') {
                        i++;
                        continue;
                    }
                    if (ch == '"') {
                        quote = '\0';
                    }
                }
            }
        }

        if (source[i] == '\0' && !clause_ended_with_esac) {
            return false;
        }

        cmd_end = i;
        memset(&clause, 0, sizeof(clause));
        clause.patterns = dup_trimmed_slice(source, pat_start, pat_end);
        clause.body = dup_trimmed_slice(source, cmd_start, cmd_end);
        clause.terminator = terminator;
        clause.body_node = parse_embedded_program_root(
            ctx, clause.body, source_offset + cmd_start, err_out);
        if (*err_out != 0) {
            return false;
        }
        ast_case_clause_vec_push(clauses_out, clause_count_out, &clause);

        if (clause_ended_with_esac || terminator == AST_CASE_TERM_END) {
            break;
        }

        if (terminator == AST_CASE_TERM_DBL_SEMI_AMP) {
            i += 3;
        } else if (terminator == AST_CASE_TERM_DBL_SEMI ||
                   terminator == AST_CASE_TERM_SEMI_AMP) {
            i += 2;
        }
    }

    return true;
}

static struct ast_node *parse_command_atom(struct parser_ctx *ctx, size_t start,
                                           size_t end, int *err_out) {
    struct ast_node *node;
    char *trimmed;
    char *cond;
    char *then_part;
    char *else_part;
    char *redir_suffix;
    char *body;
    bool is_until;
    char *name;
    char *words;
    char *fn_body;
    bool implicit_words;
    char *core;
    size_t trim_start;
    size_t trim_end;
    size_t inner_rel_start;
    size_t inner_rel_end;
    size_t redir_rel_start;

    trim_start = start;
    trim_end = end;
    while (trim_start < trim_end &&
           isspace((unsigned char)ctx->source[trim_start])) {
        trim_start++;
    }
    while (trim_end > trim_start &&
           isspace((unsigned char)ctx->source[trim_end - 1])) {
        trim_end--;
    }

    trimmed = dup_trimmed_slice(ctx->source, start, end);
    if (trimmed[0] == '\0') {
        arena_maybe_free(trimmed);
        node = ast_new_node(ctx, AST_NODE_EMPTY, start, end);
        return node;
    }

    cond = NULL;
    then_part = NULL;
    else_part = NULL;
    redir_suffix = NULL;
    body = NULL;
    name = NULL;
    words = NULL;
    fn_body = NULL;
    core = NULL;
    inner_rel_start = 0;
    inner_rel_end = 0;
    redir_rel_start = 0;
    implicit_words = false;
    is_until = false;

    if (parse_function_definition_text(trimmed, &name, &fn_body)) {
        node = ast_new_node(ctx, AST_NODE_FUNCTION_DEF, start, end);
        node->data.funcdef.name = name;
        node->data.funcdef.body = fn_body;
        node->data.funcdef.body_node =
            parse_embedded_program_root(ctx, fn_body, trim_start, err_out);
        if (*err_out != 0) {
            return NULL;
        }
    } else if (parse_simple_if(trimmed, &cond, &then_part, &else_part, &redir_suffix)) {
        node = ast_new_node(ctx, AST_NODE_IF, start, end);
        node->data.if_cmd.cond = cond;
        node->data.if_cmd.cond_node =
            parse_embedded_program_root(ctx, cond, trim_start, err_out);
        if (*err_out != 0) {
            return NULL;
        }
        node->data.if_cmd.then_part = then_part;
        node->data.if_cmd.then_node =
            parse_embedded_program_root(ctx, then_part, trim_start, err_out);
        if (*err_out != 0) {
            return NULL;
        }
        node->data.if_cmd.else_part = else_part;
        node->data.if_cmd.else_node = NULL;
        if (else_part != NULL) {
            node->data.if_cmd.else_node =
                parse_embedded_program_root(ctx, else_part, trim_start, err_out);
            if (*err_out != 0) {
                return NULL;
            }
        }
        node->data.if_cmd.redir_suffix = redir_suffix;
    } else if (parse_simple_while(trimmed, &cond, &body, &is_until,
                                  &redir_suffix)) {
        node = ast_new_node(ctx, is_until ? AST_NODE_UNTIL : AST_NODE_WHILE,
                            start, end);
        node->data.loop.cond = cond;
        node->data.loop.cond_node =
            parse_embedded_program_root(ctx, cond, trim_start, err_out);
        if (*err_out != 0) {
            return NULL;
        }
        node->data.loop.body = body;
        node->data.loop.body_node =
            parse_embedded_program_root(ctx, body, trim_start, err_out);
        if (*err_out != 0) {
            return NULL;
        }
        node->data.loop.redir_suffix = redir_suffix;
    } else if (parse_simple_for(trimmed, &name, &words, &body,
                                &implicit_words, &redir_suffix)) {
        node = ast_new_node(ctx, AST_NODE_FOR, start, end);
        node->data.for_cmd.name = name;
        node->data.for_cmd.words = words;
        node->data.for_cmd.body = body;
        node->data.for_cmd.body_node =
            parse_embedded_program_root(ctx, body, trim_start, err_out);
        if (*err_out != 0) {
            return NULL;
        }
        node->data.for_cmd.redir_suffix = redir_suffix;
        node->data.for_cmd.implicit_words = implicit_words;
    } else if (split_case_redirection_suffix(trimmed, &core, &redir_suffix)) {
        node = ast_new_node(ctx, AST_NODE_CASE, start, end);
        node->data.case_cmd.redir_suffix = redir_suffix;
        if (!parse_case_structure(ctx, core, trim_start,
                                  &node->data.case_cmd.word_expr,
                                  &node->data.case_cmd.clauses,
                                  &node->data.case_cmd.clause_count,
                                  err_out)) {
            node->kind = AST_NODE_LEGACY;
        }
        if (*err_out != 0) {
            return NULL;
        }
        arena_maybe_free(core);
    } else if (trimmed[0] != '\0' && strncmp(trimmed, "case", 4) == 0 &&
               keyword_boundary(trimmed[4])) {
        node = ast_new_node(ctx, AST_NODE_CASE, start, end);
        node->data.case_cmd.redir_suffix = arena_xstrdup("");
        if (!parse_case_structure(ctx, trimmed, trim_start,
                                  &node->data.case_cmd.word_expr,
                                  &node->data.case_cmd.clauses,
                                  &node->data.case_cmd.clause_count,
                                  err_out)) {
            node->kind = AST_NODE_LEGACY;
        }
        if (*err_out != 0) {
            return NULL;
        }
    } else if (unwrap_subshell_group(trimmed, &inner_rel_start, &inner_rel_end,
                                     &redir_rel_start)) {
        node = ast_new_node(ctx, AST_NODE_SUBSHELL, start, end);
        node->data.group.body = dup_trimmed_slice(trimmed, inner_rel_start,
                                                  inner_rel_end);
        node->data.group.redir_suffix =
            dup_trimmed_slice(trimmed, redir_rel_start, strlen(trimmed));
        node->data.group.body_node = parse_sequence(
            ctx, trim_start + inner_rel_start, trim_start + inner_rel_end,
            err_out);
        if (*err_out != 0) {
            return NULL;
        }
    } else if (unwrap_brace_group(trimmed, &inner_rel_start, &inner_rel_end,
                                  &redir_rel_start)) {
        node = ast_new_node(ctx, AST_NODE_BRACE_GROUP, start, end);
        node->data.group.body = dup_trimmed_slice(trimmed, inner_rel_start,
                                                  inner_rel_end);
        node->data.group.redir_suffix =
            dup_trimmed_slice(trimmed, redir_rel_start, strlen(trimmed));
        node->data.group.body_node = parse_sequence(
            ctx, trim_start + inner_rel_start, trim_start + inner_rel_end,
            err_out);
        if (*err_out != 0) {
            return NULL;
        }
    } else if (source_contains_heredoc_operator(trimmed)) {
        node = ast_new_node(ctx, AST_NODE_LEGACY, start, end);
    } else {
        node = ast_new_node(ctx, AST_NODE_SIMPLE_COMMAND, start, end);
        if (parse_simple_parts(ctx, trimmed, &node->data.simple.raw_words,
                               &node->data.simple.redirs) != 0) {
            *err_out = -1;
            return NULL;
        }
    }

    arena_maybe_free(trimmed);
    return node;
}

static struct ast_node *parse_pipeline(struct parser_ctx *ctx, size_t start,
                                       size_t end, int *err_out) {
    struct ast_node **items;
    size_t items_len;
    size_t i;
    size_t part_start;
    char quote;
    int paren_depth;
    int brace_depth;
    int if_depth;
    int case_depth;
    int loop_depth;
    bool negate;

    items = NULL;
    items_len = 0;
    part_start = start;
    quote = '\0';
    paren_depth = 0;
    brace_depth = 0;
    if_depth = 0;
    case_depth = 0;
    loop_depth = 0;
    negate = false;

    while (part_start < end && isspace((unsigned char)ctx->source[part_start])) {
        part_start++;
    }
    while (part_start < end && ctx->source[part_start] == '!') {
        size_t next;

        next = part_start + 1;
        while (next < end && isspace((unsigned char)ctx->source[next])) {
            next++;
        }
        negate = !negate;
        part_start = next;
    }

    start = part_start;
    for (i = start; i <= end; i++) {
        char ch;
        bool delim;

        ch = i < end ? ctx->source[i] : '\0';
        delim = false;

        if (i == end) {
            delim = true;
        } else if (shell_position_in_comment(ctx->source, ctx->len, i)) {
            continue;
        } else if (quote == '\0') {
            if (ch == '\\' && i + 1 < end) {
                if (ctx->source[i + 1] == '\n') {
                    i++;
                    continue;
                }
                i++;
                continue;
            }
            if (ch == '$' && i + 1 < end && ctx->source[i + 1] == '(') {
                size_t subst_end;

                if (find_command_subst_end(ctx->source, i, &subst_end) &&
                    subst_end < end) {
                    i = subst_end;
                    continue;
                }
            }
            if (ch == '$' && i + 1 < end && ctx->source[i + 1] == '\'') {
                size_t dq_end;

                if (find_dollar_single_quote_end(ctx->source, i, &dq_end) &&
                    dq_end < end) {
                    i = dq_end;
                    continue;
                }
            }
            if (paren_depth == 0 && brace_depth == 0 &&
                (isalpha((unsigned char)ch) || ch == '_') &&
                word_starts_command_position(ctx->source, i)) {
                size_t j;
                size_t boundary;
                char keyword[16];
                size_t kwlen;

                j = i;
                kwlen = 0;
                while (j < end) {
                    if (ctx->source[j] == '\\' && j + 1 < end &&
                        ctx->source[j + 1] == '\n') {
                        j += 2;
                        continue;
                    }
                    if (!isalnum((unsigned char)ctx->source[j]) &&
                        ctx->source[j] != '_') {
                        break;
                    }
                    if (kwlen + 1 < sizeof(keyword)) {
                        keyword[kwlen] = ctx->source[j];
                    }
                    kwlen++;
                    j++;
                }
                boundary = skip_continuations_forward(ctx->source, j);
                if (boundary < end && keyword_boundary(ctx->source[boundary]) &&
                    ctx->source[boundary] != ')') {
                    if (kwlen == 2 && strncmp(keyword, "if", 2) == 0) {
                        if_depth++;
                    } else if (kwlen == 2 && strncmp(keyword, "fi", 2) == 0 &&
                               if_depth > 0) {
                        if_depth--;
                    } else if (kwlen == 4 && strncmp(keyword, "case", 4) == 0) {
                        case_depth++;
                    } else if (kwlen == 4 && strncmp(keyword, "esac", 4) == 0 &&
                               case_depth > 0) {
                        case_depth--;
                    } else if (case_depth == 0) {
                        if ((kwlen == 5 && strncmp(keyword, "while", 5) == 0) ||
                            (kwlen == 5 && strncmp(keyword, "until", 5) == 0) ||
                            (kwlen == 3 && strncmp(keyword, "for", 3) == 0)) {
                            loop_depth++;
                        } else if (kwlen == 4 &&
                                   strncmp(keyword, "done", 4) == 0 &&
                                   loop_depth > 0 &&
                                   keyword_preceded_by_list_separator(ctx->source, i)) {
                            loop_depth--;
                        }
                    }
                }
                i = j - 1;
                continue;
            }
            if (ch == '\'' || ch == '"') {
                quote = ch;
            } else if (ch == '(') {
                paren_depth++;
            } else if (ch == ')' && paren_depth > 0) {
                paren_depth--;
            } else if (ch == '{') {
                brace_depth++;
            } else if (ch == '}' && brace_depth > 0) {
                brace_depth--;
            } else if (paren_depth == 0 && brace_depth == 0 &&
                       if_depth == 0 && case_depth == 0 && loop_depth == 0 &&
                       ch == '|' && i + 1 < end && ctx->source[i + 1] != '|' &&
                       !(i > start && ctx->source[i - 1] == '|') &&
                       !(i > start && ctx->source[i - 1] == '>')) {
                delim = true;
            }
        } else if (quote == '\'' && ch == '\'') {
            quote = '\0';
        } else if (quote == '"') {
            if (ch == '\\' && i + 1 < end) {
                i++;
                continue;
            }
            if (ch == '"') {
                quote = '\0';
            }
        }

        if (delim) {
            struct ast_node *part;
            size_t part_end;

            part_end = i;
            while (part_start < part_end &&
                   isspace((unsigned char)ctx->source[part_start])) {
                part_start++;
            }
            while (part_end > part_start &&
                   isspace((unsigned char)ctx->source[part_end - 1])) {
                part_end--;
            }
            if (part_end <= part_start) {
                report_unexpected_token(ctx, i < end ? i : end, "|");
                *err_out = -1;
                return NULL;
            }
            part = parse_command_atom(ctx, part_start, part_end, err_out);
            if (*err_out != 0) {
                return NULL;
            }
            ast_node_vec_push(&items, &items_len, part);
            part_start = i + 1;
        }
    }

    if (items_len == 1 && !negate) {
        return items[0];
    }

    {
        struct ast_node *node;

        node = ast_new_node(ctx, AST_NODE_PIPELINE, start, end);
        node->data.pipeline.items = items;
        node->data.pipeline.len = items_len;
        node->data.pipeline.negate = negate;
        return node;
    }
}

static struct ast_node *parse_andor(struct parser_ctx *ctx, size_t start,
                                    size_t end, int *err_out) {
    struct ast_node **items;
    enum ast_andor_op *ops;
    size_t items_len;
    size_t ops_len;
    size_t i;
    size_t part_start;
    char quote;
    int paren_depth;
    int brace_depth;
    int if_depth;
    int case_depth;
    int loop_depth;

    items = NULL;
    ops = NULL;
    items_len = 0;
    ops_len = 0;
    part_start = start;
    quote = '\0';
    paren_depth = 0;
    brace_depth = 0;
    if_depth = 0;
    case_depth = 0;
    loop_depth = 0;

    if (compound_needs_single_atom(dup_trimmed_slice(ctx->source, start, end))) {
        return parse_pipeline(ctx, start, end, err_out);
    }

    for (i = start; i <= end; i++) {
        char ch;
        bool delim;

        ch = i < end ? ctx->source[i] : '\0';
        delim = false;

        if (i == end) {
            delim = true;
        } else if (shell_position_in_comment(ctx->source, ctx->len, i)) {
            continue;
        } else if (quote == '\0') {
            if (ch == '\\' && i + 1 < end) {
                if (ctx->source[i + 1] == '\n') {
                    i++;
                    continue;
                }
                i++;
                continue;
            }
            if (ch == '$' && i + 1 < end && ctx->source[i + 1] == '(') {
                size_t subst_end;

                if (find_command_subst_end(ctx->source, i, &subst_end) &&
                    subst_end < end) {
                    i = subst_end;
                    continue;
                }
            }
            if (ch == '$' && i + 1 < end && ctx->source[i + 1] == '\'') {
                size_t dq_end;

                if (find_dollar_single_quote_end(ctx->source, i, &dq_end) &&
                    dq_end < end) {
                    i = dq_end;
                    continue;
                }
            }
            if (ch == '\'' || ch == '"') {
                quote = ch;
            } else if (paren_depth == 0 && brace_depth == 0 &&
                       (isalpha((unsigned char)ch) || ch == '_') &&
                       word_starts_command_position(ctx->source, i)) {
                size_t j;
                size_t boundary;
                char keyword[16];
                size_t kwlen;

                j = i;
                kwlen = 0;
                while (j < end) {
                    if (ctx->source[j] == '\\' && j + 1 < end &&
                        ctx->source[j + 1] == '\n') {
                        j += 2;
                        continue;
                    }
                    if (!isalnum((unsigned char)ctx->source[j]) &&
                        ctx->source[j] != '_') {
                        break;
                    }
                    if (kwlen + 1 < sizeof(keyword)) {
                        keyword[kwlen] = ctx->source[j];
                    }
                    kwlen++;
                    j++;
                }
                boundary = skip_continuations_forward(ctx->source, j);
                if (boundary < end && keyword_boundary(ctx->source[boundary]) &&
                    ctx->source[boundary] != ')') {
                    if (kwlen == 2 && strncmp(keyword, "if", 2) == 0) {
                        if_depth++;
                    } else if (kwlen == 2 && strncmp(keyword, "fi", 2) == 0 &&
                               if_depth > 0) {
                        if_depth--;
                    } else if (kwlen == 4 && strncmp(keyword, "case", 4) == 0) {
                        case_depth++;
                    } else if (kwlen == 4 && strncmp(keyword, "esac", 4) == 0 &&
                               case_depth > 0) {
                        case_depth--;
                    } else if (case_depth == 0) {
                        if ((kwlen == 5 && strncmp(keyword, "while", 5) == 0) ||
                            (kwlen == 5 && strncmp(keyword, "until", 5) == 0) ||
                            (kwlen == 3 && strncmp(keyword, "for", 3) == 0)) {
                            loop_depth++;
                        } else if (kwlen == 4 &&
                                   strncmp(keyword, "done", 4) == 0 &&
                                   loop_depth > 0 &&
                                   keyword_preceded_by_list_separator(ctx->source, i)) {
                            loop_depth--;
                        }
                    }
                }
                i = j - 1;
                continue;
            } else if (ch == '(') {
                paren_depth++;
            } else if (ch == ')' && paren_depth > 0) {
                paren_depth--;
            } else if (ch == '{') {
                brace_depth++;
            } else if (ch == '}' && brace_depth > 0) {
                brace_depth--;
            } else if (paren_depth == 0 && brace_depth == 0 &&
                       if_depth == 0 && case_depth == 0 && loop_depth == 0 &&
                       ch == '&' && i + 1 < end && ctx->source[i + 1] == '&') {
                delim = true;
            } else if (paren_depth == 0 && brace_depth == 0 &&
                       if_depth == 0 && case_depth == 0 && loop_depth == 0 &&
                       ch == '|' && i + 1 < end && ctx->source[i + 1] == '|') {
                delim = true;
            }
        } else if (quote == '\'' && ch == '\'') {
            quote = '\0';
        } else if (quote == '"') {
            if (ch == '\\' && i + 1 < end) {
                i++;
                continue;
            }
            if (ch == '"') {
                quote = '\0';
            }
        }

        if (delim) {
            struct ast_node *part;
            size_t part_end;

            part_end = i;
            while (part_start < part_end &&
                   isspace((unsigned char)ctx->source[part_start])) {
                part_start++;
            }
            while (part_end > part_start &&
                   isspace((unsigned char)ctx->source[part_end - 1])) {
                part_end--;
            }
            if (part_end <= part_start) {
                report_unexpected_token(ctx, i < end ? i : end,
                                        i < end && ctx->source[i] == '&' ? "&&"
                                                                          : "||");
                *err_out = -1;
                return NULL;
            }
            part = parse_pipeline(ctx, part_start, part_end, err_out);
            if (*err_out != 0) {
                return NULL;
            }
            ast_node_vec_push(&items, &items_len, part);
            if (i < end) {
                ast_andor_op_push(&ops, &ops_len,
                                  ctx->source[i] == '&' ? AST_ANDOR_AND
                                                        : AST_ANDOR_OR);
                i++;
            }
            part_start = i + 1;
        }
    }

    if (items_len == 1) {
        return items[0];
    }

    {
        struct ast_node *node;

        node = ast_new_node(ctx, AST_NODE_AND_OR, start, end);
        node->data.andor.items = items;
        node->data.andor.ops = ops;
        node->data.andor.len = items_len;
        return node;
    }
}

static struct ast_node *parse_sequence(struct parser_ctx *ctx, size_t start,
                                       size_t end, int *err_out) {
    struct ast_node **items;
    size_t items_len;
    size_t i;
    size_t part_start;
    char quote;
    int paren_depth;
    int brace_depth;
    int if_depth;
    int case_depth;
    int loop_depth;

    items = NULL;
    items_len = 0;
    part_start = start;
    quote = '\0';
    paren_depth = 0;
    brace_depth = 0;
    if_depth = 0;
    case_depth = 0;
    loop_depth = 0;

    for (i = start; i <= end; i++) {
        char ch;
        bool delim;
        bool async_delim;

        ch = i < end ? ctx->source[i] : '\0';
        delim = false;
        async_delim = false;

        if (i == end) {
            delim = true;
        } else if (shell_position_in_comment(ctx->source, ctx->len, i)) {
            if (ch == '#') {
                size_t comment_end;

                comment_end = i;
                while (comment_end < end && ctx->source[comment_end] != '\n') {
                    comment_end++;
                }
                i = comment_end == end ? comment_end : comment_end - 1;
                if (comment_end < end && paren_depth == 0 && brace_depth == 0 &&
                    if_depth == 0 && case_depth == 0 && loop_depth == 0) {
                    delim = true;
                }
            }
        } else if (quote == '\0') {
            if (ch == '\\' && i + 1 < end) {
                if (ctx->source[i + 1] == '\n') {
                    i++;
                    continue;
                }
                i++;
                continue;
            }
            if (ch == '$' && i + 1 < end && ctx->source[i + 1] == '(') {
                size_t subst_end;

                if (find_command_subst_end(ctx->source, i, &subst_end) &&
                    subst_end < end) {
                    i = subst_end;
                    continue;
                }
            }
            if (ch == '$' && i + 1 < end && ctx->source[i + 1] == '\'') {
                size_t dq_end;

                if (find_dollar_single_quote_end(ctx->source, i, &dq_end) &&
                    dq_end < end) {
                    i = dq_end;
                    continue;
                }
            }
            if (ch == '\'' || ch == '"') {
                quote = ch;
            } else if (paren_depth == 0 && brace_depth == 0 &&
                       (isalpha((unsigned char)ch) || ch == '_') &&
                       word_starts_command_position(ctx->source, i)) {
                size_t j;
                size_t boundary;
                char keyword[16];
                size_t kwlen;

                j = i;
                kwlen = 0;
                while (j < end) {
                    if (ctx->source[j] == '\\' && j + 1 < end &&
                        ctx->source[j + 1] == '\n') {
                        j += 2;
                        continue;
                    }
                    if (!isalnum((unsigned char)ctx->source[j]) &&
                        ctx->source[j] != '_') {
                        break;
                    }
                    if (kwlen + 1 < sizeof(keyword)) {
                        keyword[kwlen] = ctx->source[j];
                    }
                    kwlen++;
                    j++;
                }
                boundary = skip_continuations_forward(ctx->source, j);
                if (boundary < end && keyword_boundary(ctx->source[boundary]) &&
                    ctx->source[boundary] != ')') {
                    if (kwlen == 2 && strncmp(keyword, "if", 2) == 0) {
                        if_depth++;
                    } else if (kwlen == 2 && strncmp(keyword, "fi", 2) == 0 &&
                               if_depth > 0) {
                        if_depth--;
                    } else if (kwlen == 4 && strncmp(keyword, "case", 4) == 0) {
                        case_depth++;
                    } else if (kwlen == 4 && strncmp(keyword, "esac", 4) == 0 &&
                               case_depth > 0) {
                        case_depth--;
                    } else if (case_depth == 0) {
                        if ((kwlen == 5 && strncmp(keyword, "while", 5) == 0) ||
                            (kwlen == 5 && strncmp(keyword, "until", 5) == 0) ||
                            (kwlen == 3 && strncmp(keyword, "for", 3) == 0)) {
                            loop_depth++;
                        } else if (kwlen == 4 &&
                                   strncmp(keyword, "done", 4) == 0 &&
                                   loop_depth > 0 &&
                                   keyword_preceded_by_list_separator(ctx->source, i)) {
                            loop_depth--;
                        }
                    }
                }
                i = j - 1;
                continue;
            } else if (ch == '(') {
                paren_depth++;
            } else if (ch == ')' && paren_depth > 0) {
                paren_depth--;
            } else if (ch == '{') {
                brace_depth++;
            } else if (ch == '}' && brace_depth > 0) {
                brace_depth--;
            } else if (paren_depth == 0 && brace_depth == 0 &&
                       if_depth == 0 && case_depth == 0 && loop_depth == 0 &&
                       ch == ';') {
                delim = true;
            } else if (paren_depth == 0 && brace_depth == 0 &&
                       if_depth == 0 && case_depth == 0 && loop_depth == 0 &&
                       ch == '\n' && !newline_continues_command(ctx->source, i)) {
                delim = true;
            } else if (paren_depth == 0 && brace_depth == 0 &&
                       if_depth == 0 && case_depth == 0 && loop_depth == 0 &&
                       is_async_separator_amp(ctx->source, i)) {
                delim = true;
                async_delim = true;
            }
        } else if (quote == '\'' && ch == '\'') {
            quote = '\0';
        } else if (quote == '"') {
            if (ch == '\\' && i + 1 < end) {
                i++;
                continue;
            }
            if (ch == '"') {
                quote = '\0';
            }
        }

        if (delim) {
            struct ast_node *part;
            size_t part_end;

            part_end = i;
            while (part_start < part_end &&
                   isspace((unsigned char)ctx->source[part_start])) {
                part_start++;
            }
            while (part_end > part_start &&
                   isspace((unsigned char)ctx->source[part_end - 1])) {
                part_end--;
            }
            if (part_end > part_start) {
                part = parse_andor(ctx, part_start, part_end, err_out);
                if (*err_out != 0) {
                    return NULL;
                }
                if (async_delim) {
                    struct ast_node *async_node;

                    async_node = ast_new_node(ctx, AST_NODE_ASYNC, part_start, i + 1);
                    async_node->data.unary.child = part;
                    part = async_node;
                }
                ast_node_vec_push(&items, &items_len, part);
            } else if (async_delim) {
                report_unexpected_token(ctx, i, "&");
                *err_out = -1;
                return NULL;
            }
            part_start = i + 1;
        }
    }

    if (items_len == 0) {
        return ast_new_node(ctx, AST_NODE_EMPTY, start, end);
    }
    if (items_len == 1) {
        return items[0];
    }

    {
        struct ast_node *node;

        node = ast_new_node(ctx, AST_NODE_SEQUENCE, start, end);
        node->data.list.items = items;
        node->data.list.len = items_len;
        return node;
    }
}

int parse_program_at(const char *source_name, size_t base_line,
                     const char *source, struct ast_program **out_program) {
    struct ast_program *program;
    struct parser_ctx ctx;
    int err;

    program = arena_xmalloc(sizeof(*program));
    memset(program, 0, sizeof(*program));
    program->source = arena_xstrdup(source);

    ctx.source_name = source_name;
    ctx.base_line = base_line;
    ctx.source = program->source;
    ctx.len = strlen(program->source);

    err = 0;
    if (source_contains_heredoc_operator(program->source)) {
        program->root = ast_new_node(&ctx, AST_NODE_LEGACY, 0, ctx.len);
    } else {
        program->root = parse_sequence(&ctx, 0, ctx.len, &err);
    }
    if (err != 0) {
        return -1;
    }

    *out_program = program;
    return 0;
}

int parse_program(const char *source, struct ast_program **out_program) {
    return parse_program_at(NULL, 1, source, out_program);
}

void ast_program_free(struct ast_program *program) {
    (void)program;
    /*
     * AST program objects are allocated from command/script arenas and are
     * released via arena mark-rewind/reset at the execution boundary.
     */
}
