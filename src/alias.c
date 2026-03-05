/* SPDX-License-Identifier: 0BSD */

/* posish - alias expansion */

#include "alias.h"
#include "arena.h"
#include "shell.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* ---------- forward declarations ---------- */

static bool find_command_subst_end_alias(const char *source, size_t start, size_t *out_end);
static bool find_dollar_single_quote_end_alias(const char *source, size_t start, size_t *out_end);

/* ---------- helpers ---------- */

static char *alias_env_key(const char *name) {
    size_t len;
    char *key;

    len = strlen(name);
    key = arena_xmalloc(len + 14);
    memcpy(key, "POSISH_ALIAS_", 13);
    memcpy(key + 13, name, len + 1);
    return key;
}

char *alias_lookup_dup(const char *name) {
    char *key;
    const char *value;
    char *dup;

    if (name == NULL || name[0] == '\0') {
        return NULL;
    }

    key = alias_env_key(name);
    value = getenv(key);
    arena_maybe_free(key);
    if (value == NULL) {
        return NULL;
    }

    dup = arena_xstrdup(value);
    return dup;
}

static bool alias_value_has_trailing_blank(const char *value) {
    size_t len;

    if (value == NULL || value[0] == '\0') {
        return false;
    }

    len = strlen(value);
    return len > 0 && (value[len - 1] == ' ' || value[len - 1] == '\t');
}

static bool is_plain_command_word_for_alias(const char *word) {
    size_t i;

    if (word == NULL || word[0] == '\0') {
        return false;
    }
    if (strchr(word, '/') != NULL) {
        return false;
    }

    for (i = 0; word[i] != '\0'; i++) {
        if (word[i] == '\'' || word[i] == '"' || word[i] == '\\' ||
            word[i] == '$' || word[i] == '`') {
            return false;
        }
    }
    return true;
}

/* ---------- token vec ---------- */

struct alias_token {
    char *leading_ws;
    char *text;
    char *suppress_alias;
    bool force_expand;
    bool is_operator;
};

struct alias_token_vec {
    struct alias_token *items;
    size_t len;
};

static void alias_token_vec_free(struct alias_token_vec *vec) {
    size_t i;

    for (i = 0; i < vec->len; i++) {
        arena_maybe_free(vec->items[i].leading_ws);
        arena_maybe_free(vec->items[i].text);
        arena_maybe_free(vec->items[i].suppress_alias);
    }
    arena_maybe_free(vec->items);
    vec->items = NULL;
    vec->len = 0;
}

static int alias_token_vec_push(struct alias_token_vec *vec, const char *leading_ws,
                                size_t leading_ws_len, const char *text, size_t len,
                                bool is_operator, const char *suppress_alias) {
    char *leading_copy;
    char *copy;
    char *suppress_copy;

    leading_copy = arena_xmalloc(leading_ws_len + 1);
    memcpy(leading_copy, leading_ws, leading_ws_len);
    leading_copy[leading_ws_len] = '\0';

    copy = arena_xmalloc(len + 1);
    memcpy(copy, text, len);
    copy[len] = '\0';
    suppress_copy = NULL;
    if (suppress_alias != NULL) {
        suppress_copy = arena_xstrdup(suppress_alias);
    }
    vec->items = arena_xrealloc(vec->items, sizeof(*vec->items) * (vec->len + 1));
    vec->items[vec->len].leading_ws = leading_copy;
    vec->items[vec->len].text = copy;
    vec->items[vec->len].suppress_alias = suppress_copy;
    vec->items[vec->len].force_expand = false;
    vec->items[vec->len].is_operator = is_operator;
    vec->len++;
    return 0;
}

/* ---------- operator helpers ---------- */

static size_t alias_operator_len_at(const char *s, size_t pos) {
    if (s[pos] == '!') {
        bool next_boundary;
        bool prev_boundary;

        prev_boundary = pos == 0 || isspace((unsigned char)s[pos - 1]) ||
                        s[pos - 1] == ';' || s[pos - 1] == '&' ||
                        s[pos - 1] == '|' || s[pos - 1] == '(' ||
                        s[pos - 1] == ')' || s[pos - 1] == '{' ||
                        s[pos - 1] == '}' || s[pos - 1] == '<' ||
                        s[pos - 1] == '>';
        next_boundary = s[pos + 1] == '\0' ||
                        isspace((unsigned char)s[pos + 1]) ||
                        s[pos + 1] == ';' || s[pos + 1] == '&' ||
                        s[pos + 1] == '|' || s[pos + 1] == '(' ||
                        s[pos + 1] == ')' || s[pos + 1] == '{' ||
                        s[pos + 1] == '}';
        if (prev_boundary && next_boundary) {
            return 1;
        }
        return 0;
    }

    if (s[pos] == '\0') {
        return 0;
    }
    if (s[pos] == '<' && s[pos + 1] == '<' && s[pos + 2] == '-') {
        return 3;
    }
    if ((s[pos] == '&' && s[pos + 1] == '&') ||
        (s[pos] == '|' && s[pos + 1] == '|') ||
        (s[pos] == ';' && s[pos + 1] == ';') ||
        (s[pos] == '<' && s[pos + 1] == '<') ||
        (s[pos] == '>' && s[pos + 1] == '>') ||
        (s[pos] == '<' && s[pos + 1] == '&') ||
        (s[pos] == '>' && s[pos + 1] == '&') ||
        (s[pos] == '<' && s[pos + 1] == '>')) {
        return 2;
    }
    if (s[pos] == ';' || s[pos] == '|' || s[pos] == '&' || s[pos] == '(' ||
        s[pos] == ')' || s[pos] == '{' || s[pos] == '}' || s[pos] == '<' ||
        s[pos] == '>') {
        return 1;
    }
    return 0;
}

static bool alias_op_is_redirection(const char *op) {
    return strcmp(op, "<") == 0 || strcmp(op, ">") == 0 ||
           strcmp(op, "<<") == 0 || strcmp(op, "<<-") == 0 ||
           strcmp(op, ">>") == 0 || strcmp(op, "<&") == 0 ||
           strcmp(op, ">&") == 0 || strcmp(op, "<>") == 0;
}

static bool alias_word_opens_command_position(const char *word) {
    return strcmp(word, "then") == 0 || strcmp(word, "do") == 0 ||
           strcmp(word, "else") == 0 || strcmp(word, "elif") == 0;
}

static bool alias_word_is_io_number(const char *word) {
    size_t i;

    if (word == NULL || word[0] == '\0') {
        return false;
    }
    for (i = 0; word[i] != '\0'; i++) {
        if (!isdigit((unsigned char)word[i])) {
            return false;
        }
    }
    return true;
}

static bool alias_word_is_reserved_in_context(const char *word,
                                              bool command_position,
                                              int for_state,
                                              int case_state) {
    if (!command_position) {
        return false;
    }
    if (for_state == 1) {
        return false;
    }
    if (case_state == 1) {
        return true;
    }
    if (for_state == 2) {
        return strcmp(word, "in") == 0 || strcmp(word, "do") == 0;
    }
    if (for_state == 3) {
        return strcmp(word, "do") == 0;
    }
    if (for_state == 4 && strcmp(word, "done") == 0) {
        return true;
    }
    if (case_state == 2) {
        return strcmp(word, "in") == 0;
    }
    if (case_state == 3) {
        return strcmp(word, "esac") == 0;
    }

    return strcmp(word, "if") == 0 || strcmp(word, "then") == 0 ||
           strcmp(word, "elif") == 0 || strcmp(word, "else") == 0 ||
           strcmp(word, "fi") == 0 || strcmp(word, "while") == 0 ||
           strcmp(word, "until") == 0 || strcmp(word, "for") == 0 ||
           strcmp(word, "in") == 0 || strcmp(word, "do") == 0 ||
           strcmp(word, "done") == 0 || strcmp(word, "case") == 0 ||
           strcmp(word, "esac") == 0;
}

static bool alias_chain_contains(const char *chain, const char *name) {
    size_t name_len;
    const char *p;

    if (chain == NULL || name == NULL) {
        return false;
    }
    name_len = strlen(name);
    p = chain;
    while (*p != '\0') {
        const char *eol;
        size_t seg_len;

        eol = strchr(p, '\n');
        if (eol == NULL) {
            seg_len = strlen(p);
        } else {
            seg_len = (size_t)(eol - p);
        }
        if (seg_len == name_len && strncmp(p, name, name_len) == 0) {
            return true;
        }
        if (eol == NULL) {
            break;
        }
        p = eol + 1;
    }
    return false;
}

static char *alias_chain_append(const char *chain, const char *name) {
    size_t chain_len;
    size_t name_len;
    char *out;

    if (name == NULL || name[0] == '\0') {
        return chain != NULL ? arena_xstrdup(chain) : NULL;
    }
    if (chain == NULL || chain[0] == '\0') {
        return arena_xstrdup(name);
    }

    chain_len = strlen(chain);
    name_len = strlen(name);
    out = arena_xmalloc(chain_len + 1 + name_len + 1);
    memcpy(out, chain, chain_len);
    out[chain_len] = '\n';
    memcpy(out + chain_len + 1, name, name_len + 1);
    return out;
}

static bool alias_find_backquote_end(const char *source, size_t start,
                                     size_t *out_end) {
    size_t i;

    if (source[start] != '`') {
        return false;
    }

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

static bool find_command_subst_end_alias(const char *source, size_t start,
                                         size_t *out_end) {
    size_t depth;
    size_t i;
    char quote;

    if (source[start] != '$' || source[start + 1] != '(') {
        return false;
    }

    depth = 1;
    i = start + 2;
    quote = '\0';

    while (source[i] != '\0') {
        if (quote == '\'') {
            if (source[i] == '\'') quote = '\0';
            i++;
            continue;
        }
        if (quote == '"') {
            if (source[i] == '\\' && source[i+1] != '\0') { i += 2; continue; }
            if (source[i] == '"') quote = '\0';
            i++;
            continue;
        }
        if (source[i] == '\\' && source[i+1] != '\0') { i += 2; continue; }
        if (source[i] == '\'' || source[i] == '"') { quote = source[i++]; continue; }
        if (source[i] == '(' || (source[i] == '$' && source[i+1] == '(')) {
            if (source[i] == '$') i++;
            depth++;
            i++;
            continue;
        }
        if (source[i] == ')') {
            depth--;
            if (depth == 0) {
                *out_end = i;
                return true;
            }
        }
        i++;
    }
    return false;
}

static bool find_dollar_single_quote_end_alias(const char *source, size_t start,
                                               size_t *out_end) {
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
            *out_end = i;
            return true;
        }
        i++;
    }
    return false;
}

/* ---------- collapse line continuations (needed for tokenizer) ---------- */

static char *alias_collapse_line_continuations(const char *source) {
    size_t i;
    size_t j;
    size_t slen;
    char quote;
    char *out;

    slen = strlen(source);
    out = arena_xmalloc(slen + 1);
    quote = '\0';
    j = 0;

    for (i = 0; source[i] != '\0'; i++) {
        char ch;

        ch = source[i];
        if (quote == '\'') {
            out[j++] = ch;
            if (ch == '\'') {
                quote = '\0';
            }
            continue;
        }

        if (ch == '\\' && source[i + 1] == '\n') {
            i++;
            continue;
        }

        if (quote == '"') {
            out[j++] = ch;
            if (ch == '\\' && source[i + 1] != '\0' && source[i + 1] != '\n') {
                out[j++] = source[++i];
                continue;
            }
            if (ch == '"') {
                quote = '\0';
            }
            continue;
        }

        out[j++] = ch;
        if (ch == '\\' && source[i + 1] != '\0') {
            out[j++] = source[++i];
            continue;
        }
        if (ch == '\'' || ch == '"') {
            quote = ch;
        }
    }

    out[j] = '\0';
    return out;
}

/* ---------- tokenizer ---------- */

static int alias_tokenize_with_suppress(const char *text,
                                        const char *suppress_alias,
                                        struct alias_token_vec *out) {
    char *normalized;
    const char *input;
    size_t i;

    out->items = NULL;
    out->len = 0;
    normalized = alias_collapse_line_continuations(text);
    input = normalized;
    i = 0;

    while (input[i] != '\0') {
        size_t ws_start;
        size_t ws_len;
        size_t op_len;
        size_t start;
        char quote;

        ws_start = i;
        while (isspace((unsigned char)input[i])) {
            i++;
        }
        ws_len = i - ws_start;
        if (input[i] == '\0') {
            break;
        }

        op_len = alias_operator_len_at(input, i);
        if (op_len > 0) {
            if (alias_token_vec_push(out, input + ws_start, ws_len, input + i, op_len,
                                     true, suppress_alias) != 0) {
                alias_token_vec_free(out);
                arena_maybe_free(normalized);
                return -1;
            }
            i += op_len;
            continue;
        }

        start = i;
        quote = '\0';
        while (input[i] != '\0') {
            if (quote == '\0') {
                op_len = alias_operator_len_at(input, i);
                if (op_len > 0 || isspace((unsigned char)input[i])) {
                    break;
                }
                if (input[i] == '\'' || input[i] == '"') {
                    quote = input[i++];
                    continue;
                }
                if (input[i] == '\\' && input[i + 1] != '\0') {
                    i += 2;
                    continue;
                }
                if (input[i] == '$' && input[i + 1] == '(') {
                    size_t end;
                    if (find_command_subst_end_alias(input, i, &end)) {
                        i = end + 1;
                        continue;
                    }
                }
                if (input[i] == '$' && input[i + 1] == '\'') {
                    size_t end;
                    if (find_dollar_single_quote_end_alias(input, i, &end)) {
                        i = end + 1;
                        continue;
                    }
                }
                if (input[i] == '`') {
                    size_t end;
                    if (alias_find_backquote_end(input, i, &end)) {
                        i = end + 1;
                        continue;
                    }
                }
            } else if (quote == '\'') {
                if (input[i] == '\'') {
                    quote = '\0';
                }
            } else if (quote == '"') {
                if (input[i] == '\\' && input[i + 1] != '\0') {
                    i += 2;
                    continue;
                }
                if (input[i] == '$' && input[i + 1] == '(') {
                    size_t end;
                    if (find_command_subst_end_alias(input, i, &end)) {
                        i = end + 1;
                        continue;
                    }
                }
                if (input[i] == '"') {
                    quote = '\0';
                }
            }
            i++;
        }

        if (i > start) {
            if (alias_token_vec_push(out, input + ws_start, ws_len, input + start,
                                     i - start, false, suppress_alias) != 0) {
                alias_token_vec_free(out);
                arena_maybe_free(normalized);
                return -1;
            }
        }
    }

    arena_maybe_free(normalized);
    return 0;
}

static int alias_tokenize(const char *text, struct alias_token_vec *out) {
    return alias_tokenize_with_suppress(text, NULL, out);
}

static int alias_token_vec_replace(struct alias_token_vec *vec, size_t index,
                                   const struct alias_token_vec *replacement) {
    struct alias_token *new_items;
    struct alias_token *old_items;
    char *old_leading_ws;
    size_t prefix_count;
    size_t old_len;
    size_t tail_count;
    size_t new_len;
    size_t r;
    char *old_text;
    char *old_suppress_alias;

    old_items = vec->items;
    old_len = vec->len;
    tail_count = old_len - (index + 1);
    prefix_count = index;
    new_len = old_len - 1 + replacement->len;
    old_leading_ws = old_items[index].leading_ws;
    old_text = old_items[index].text;
    old_suppress_alias = old_items[index].suppress_alias;

    if (new_len == 0) {
        new_items = NULL;
    } else {
        new_items = arena_xmalloc(sizeof(*new_items) * new_len);
        if (prefix_count > 0) {
            memcpy(new_items, old_items, sizeof(*new_items) * prefix_count);
        }
        if (tail_count > 0) {
            memcpy(new_items + index + replacement->len, old_items + index + 1,
                   sizeof(*new_items) * tail_count);
        }
    }

    for (r = 0; r < replacement->len; r++) {
        if (r == 0) {
            size_t old_ws_len;
            size_t repl_ws_len;
            char *combined_ws;

            old_ws_len = strlen(old_leading_ws);
            repl_ws_len = strlen(replacement->items[r].leading_ws);
            combined_ws = arena_xmalloc(old_ws_len + repl_ws_len + 1);
            memcpy(combined_ws, old_leading_ws, old_ws_len);
            memcpy(combined_ws + old_ws_len, replacement->items[r].leading_ws,
                   repl_ws_len + 1);
            new_items[index + r].leading_ws = combined_ws;
        } else {
            new_items[index + r].leading_ws =
                arena_xstrdup(replacement->items[r].leading_ws);
        }
        new_items[index + r].text = arena_xstrdup(replacement->items[r].text);
        if (replacement->items[r].suppress_alias != NULL) {
            new_items[index + r].suppress_alias =
                arena_xstrdup(replacement->items[r].suppress_alias);
        } else {
            new_items[index + r].suppress_alias = NULL;
        }
        new_items[index + r].force_expand = replacement->items[r].force_expand;
        new_items[index + r].is_operator = replacement->items[r].is_operator;
    }
    if (replacement->len == 0 && tail_count > 0) {
        size_t old_ws_len;
        size_t next_ws_len;
        char *combined_ws;

        old_ws_len = strlen(old_leading_ws);
        next_ws_len = strlen(new_items[index].leading_ws);
        combined_ws = arena_xmalloc(old_ws_len + next_ws_len + 1);
        memcpy(combined_ws, old_leading_ws, old_ws_len);
        memcpy(combined_ws + old_ws_len, new_items[index].leading_ws,
               next_ws_len + 1);
        arena_maybe_free(new_items[index].leading_ws);
        new_items[index].leading_ws = combined_ws;
    }
    vec->items = new_items;
    vec->len = new_len;
    arena_maybe_free(old_items);
    arena_maybe_free(old_leading_ws);
    arena_maybe_free(old_text);
    arena_maybe_free(old_suppress_alias);
    return 0;
}

static char *alias_render_tokens(const struct alias_token_vec *tokens) {
    size_t i;
    size_t total;
    char *out;
    size_t pos;

    total = 1;
    for (i = 0; i < tokens->len; i++) {
        total += strlen(tokens->items[i].leading_ws);
        total += strlen(tokens->items[i].text);
    }

    out = arena_xmalloc(total);
    pos = 0;
    for (i = 0; i < tokens->len; i++) {
        size_t ws_len;
        size_t tlen;

        ws_len = strlen(tokens->items[i].leading_ws);
        memcpy(out + pos, tokens->items[i].leading_ws, ws_len);
        pos += ws_len;
        tlen = strlen(tokens->items[i].text);
        memcpy(out + pos, tokens->items[i].text, tlen);
        pos += tlen;
    }
    out[pos] = '\0';
    return out;
}

/* ---------- public helpers ---------- */

static bool alias_is_name_start_char(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
}

static bool alias_is_name_char(char ch) {
    return alias_is_name_start_char(ch) || (ch >= '0' && ch <= '9');
}

bool alias_is_assignment_word(const char *word) {
    size_t i;

    if (word[0] == '\0' || !alias_is_name_start_char(word[0])) {
        return false;
    }

    i = 1;
    while (word[i] != '\0' && word[i] != '=') {
        if (!alias_is_name_char(word[i])) {
            return false;
        }
        i++;
    }

    return word[i] == '=';
}

/* ---------- public: rewrite aliases for a snippet ---------- */

int alias_rewrite_snippet(struct shell_state *state, const char *text,
                                       char **out, bool *changed) {
    struct alias_token_vec tokens;
    size_t i;
    bool command_position;
    bool expect_redir_operand;
    bool heredoc_operand_position;
    bool trailing_alias_blank;
    bool case_clause_commands;
    bool case_after_clause_sep;
    int for_state;
    int case_state;

    (void)state;

    if (alias_tokenize(text, &tokens) != 0) {
        return -1;
    }

    *changed = false;
    command_position = true;
    expect_redir_operand = false;
    heredoc_operand_position = false;
    trailing_alias_blank = false;
    case_clause_commands = false;
    case_after_clause_sep = false;
    for_state = 0;
    case_state = 0;
    i = 0;

    while (i < tokens.len) {
        bool forced_alias_token;

        if (tokens.items[i].is_operator) {
            if (case_state == 3) {
                if (strcmp(tokens.items[i].text, ")") == 0 &&
                    !case_clause_commands) {
                    case_clause_commands = true;
                } else if (strcmp(tokens.items[i].text, ";;") == 0) {
                    case_clause_commands = false;
                    case_after_clause_sep = true;
                }
            }
            if (alias_op_is_redirection(tokens.items[i].text)) {
                expect_redir_operand = true;
                heredoc_operand_position = strcmp(tokens.items[i].text, "<<") == 0 ||
                                           strcmp(tokens.items[i].text, "<<-") == 0;
            } else {
                command_position = true;
                expect_redir_operand = false;
                heredoc_operand_position = false;
                trailing_alias_blank = false;
            }
            i++;
            continue;
        }

        forced_alias_token = tokens.items[i].force_expand;
        if (expect_redir_operand) {
            if (heredoc_operand_position) {
                expect_redir_operand = false;
                heredoc_operand_position = false;
                command_position = true;
            } else {
                expect_redir_operand = false;
                heredoc_operand_position = false;
                i++;
                continue;
            }
        }

        if (tokens.items[i].force_expand) {
            command_position = true;
            tokens.items[i].force_expand = false;
        }
        if (!command_position &&
            strchr(tokens.items[i].leading_ws, '\n') != NULL) {
            command_position = true;
        }
        if (!command_position && case_state == 3 && !case_clause_commands &&
            tokens.items[i].suppress_alias == NULL && !case_after_clause_sep) {
            command_position = true;
        }

        if (!command_position) {
            if (alias_word_opens_command_position(tokens.items[i].text)) {
                command_position = true;
            }
            goto state_update;
        }

        if (alias_is_assignment_word(tokens.items[i].text)) {
            i++;
            continue;
        }

        if (alias_word_is_io_number(tokens.items[i].text) && i + 1 < tokens.len &&
            tokens.items[i + 1].is_operator &&
            alias_op_is_redirection(tokens.items[i + 1].text)) {
            expect_redir_operand = true;
            i++;
            continue;
        }

        {
            bool reserved_here;

            reserved_here = alias_word_is_reserved_in_context(tokens.items[i].text,
                                                              command_position,
                                                              for_state,
                                                              case_state);

            if (!reserved_here &&
                is_plain_command_word_for_alias(tokens.items[i].text)) {
                bool blocked;
                bool disallow_case_pattern_alias;
                char *alias_value;

                /*
                 * In case-pattern position, avoid re-aliasing plain pattern
                 * words unless this token was explicitly forced by a trailing-
                 * blank alias expansion.
                 */
                disallow_case_pattern_alias =
                    case_state == 3 && !case_clause_commands &&
                    !forced_alias_token;
                if (!disallow_case_pattern_alias) {
                    blocked = alias_chain_contains(tokens.items[i].suppress_alias,
                                                   tokens.items[i].text);
                    if (!blocked) {
                        alias_value = alias_lookup_dup(tokens.items[i].text);
                        if (alias_value != NULL) {
                            char *next_chain;
                            struct alias_token_vec repl;

                            next_chain =
                                alias_chain_append(tokens.items[i].suppress_alias,
                                                   tokens.items[i].text);
                            if (alias_tokenize_with_suppress(alias_value,
                                                             next_chain,
                                                             &repl) != 0) {
                                arena_maybe_free(next_chain);
                                arena_maybe_free(alias_value);
                                alias_token_vec_free(&tokens);
                                return -1;
                            }
                            arena_maybe_free(next_chain);
                            trailing_alias_blank =
                                alias_value_has_trailing_blank(alias_value);
                            arena_maybe_free(alias_value);

                            if (repl.len == 0) {
                                alias_token_vec_replace(&tokens, i, &repl);
                            } else {
                                alias_token_vec_replace(&tokens, i, &repl);
                            }
                            if (trailing_alias_blank && i + repl.len < tokens.len) {
                                tokens.items[i + repl.len].force_expand = true;
                            }
                            alias_token_vec_free(&repl);
                            *changed = true;
                            command_position = true;
                            expect_redir_operand = false;
                            continue;
                        }
                    }
                }
            }
        }

state_update:
        if (for_state == 0 && command_position &&
            strcmp(tokens.items[i].text, "for") == 0) {
            for_state = 1;
        } else if (for_state == 1) {
            for_state = 2;
        } else if (for_state == 2 &&
                   strcmp(tokens.items[i].text, "in") == 0) {
            for_state = 3;
        } else if (for_state == 2 &&
                   strcmp(tokens.items[i].text, "do") == 0) {
            for_state = 4;
        } else if (for_state == 3 &&
                   strcmp(tokens.items[i].text, "do") == 0) {
            for_state = 4;
        } else if (for_state == 4 &&
                   strcmp(tokens.items[i].text, "done") == 0) {
            for_state = 0;
        }

        if (case_state == 0 && command_position &&
            strcmp(tokens.items[i].text, "case") == 0) {
            case_state = 1;
        } else if (case_state == 1) {
            case_state = 2;
        } else if (case_state == 2 &&
                   strcmp(tokens.items[i].text, "in") == 0) {
            case_state = 3;
            case_clause_commands = false;
            case_after_clause_sep = false;
        } else if (case_state == 3 &&
                   strcmp(tokens.items[i].text, "esac") == 0) {
            case_state = 0;
            case_clause_commands = false;
            case_after_clause_sep = false;
        }
        if (case_state == 3 && !case_clause_commands) {
            case_after_clause_sep = false;
        }

        if (alias_word_opens_command_position(tokens.items[i].text)) {
            command_position = true;
            trailing_alias_blank = false;
        } else if (trailing_alias_blank) {
            command_position = true;
            trailing_alias_blank = false;
        } else {
            command_position = false;
        }
        i++;
    }

    *out = alias_render_tokens(&tokens);
    alias_token_vec_free(&tokens);
    return 0;
}

