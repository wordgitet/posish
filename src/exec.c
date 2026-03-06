/* SPDX-License-Identifier: 0BSD */

/* posish - execution engine */

#include "exec.h"

#include "alias.h"
#include "arena.h"
#include "builtins/builtin.h"
#include "case_command.h"
#include "compound_parse.h"
#include "error.h"
#include "expand.h"
#include "heredoc_command.h"
#include "jobs.h"
#include "lexer.h"
#include "redir.h"
#include "signals.h"
#include "trace.h"
#include "vars.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

enum andor_op { ANDOR_AND, ANDOR_OR };

struct word_vec {
    char **items;
    size_t len;
};

struct env_restore {
    char *name;
    char *old_value;
    bool existed;
    bool was_unexported;
};

struct env_restore_vec {
    struct env_restore *items;
    size_t len;
};

struct positional_backup {
    char **params;
    size_t count;
};

static int execute_program_text(struct shell_state *state, const char *source);
static int execute_program_text_internal(struct shell_state *state,
                                         const char *source,
                                         bool apply_aliases);
static int execute_andor(struct shell_state *state, const char *source);
static bool is_assignment_word(const char *word);
static bool find_command_subst_end(const char *source, size_t start,
                                   size_t *out_end);
static bool find_dollar_single_quote_end(const char *source, size_t start,
                                         size_t *out_end);
static bool command_text_needs_more_input(const char *source,
                                          bool include_heredoc);
static bool looks_like_function_header_only(const char *source);

static void exit_shell_child_status(int status) {
    int signo;

    if (shell_status_should_relay_signal(status, &signo)) {
        signal(signo, SIG_DFL);
        raise(signo);
    }
    _exit(status);
}

static void *xrealloc(void *ptr, size_t size) {
    return arena_xrealloc(ptr, size);
}

static char *dup_trimmed_slice(const char *src, size_t start, size_t end) {
    char *out;
    size_t len;

    while (start < end && isspace((unsigned char)src[start])) {
        start++;
    }
    while (end > start && isspace((unsigned char)src[end - 1])) {
        end--;
    }

    len = end - start;
    out = arena_xmalloc(len + 1);
    if (len > 0) {
        memcpy(out, src + start, len);
    }
    out[len] = '\0';
    return out;
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

static char *collapse_line_continuations(const char *source) {
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

        /*
         * POSIX line continuation removes backslash-newline before tokenization
         * (outside single quotes).
         */
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

static size_t source_line_at_offset(const char *source, size_t offset) {
    size_t i;
    size_t line;

    line = 1;
    for (i = 0; source[i] != '\0' && i < offset; i++) {
        if (source[i] == '\n') {
            line++;
        }
    }
    return line;
}

static void set_lineno_for_command(const char *source, size_t start) {
    const char *base_text;
    char *end;
    unsigned long base;
    char line_buf[32];
    size_t line;

    base = 0;
    base_text = getenv("POSISH_LINENO_BASE");
    if (base_text != NULL && base_text[0] != '\0') {
        errno = 0;
        base = strtoul(base_text, &end, 10);
        if (errno != 0 || end == base_text || *end != '\0') {
            base = 0;
        }
    }

    line = source_line_at_offset(source, start) + (size_t)base;
    snprintf(line_buf, sizeof(line_buf), "%zu", line);
    (void)setenv("LINENO", line_buf, 1);
}

static void free_string_vec(char **vec, size_t len) {
    size_t i;

    for (i = 0; i < len; i++) {
        arena_maybe_free(vec[i]);
    }
    arena_maybe_free(vec);
}

static void word_vec_free(struct word_vec *words) {
    arena_maybe_free(words->items);
    words->items = NULL;
    words->len = 0;
}

static void env_restore_vec_free(struct env_restore_vec *restore) {
    size_t i;

    for (i = 0; i < restore->len; i++) {
        arena_maybe_free(restore->items[i].name);
        arena_maybe_free(restore->items[i].old_value);
    }
    arena_maybe_free(restore->items);
    restore->items = NULL;
    restore->len = 0;
}


static void trace_simple_words(struct shell_state *state, char *const words[],
                               size_t count) {
    const char *raw_ps4;
    const char *ps4;
    char *expanded_ps4;
    struct token_vec in;
    struct token_vec out;
    size_t i;

    if (!state->xtrace || count == 0) {
        return;
    }

    raw_ps4 = getenv("PS4");
    if (raw_ps4 == NULL) {
        raw_ps4 = "+ ";
    }
    ps4 = raw_ps4;
    expanded_ps4 = NULL;

    in.items = (char **)&raw_ps4;
    in.len = 1;
    out.items = NULL;
    out.len = 0;
    if (expand_words(&in, &out, state, false) == 0) {
        if (out.len == 1) {
            expanded_ps4 = out.items[0];
            ps4 = expanded_ps4;
        } else {
            lexer_free_tokens(&out);
        }
    }

    fputs(ps4, stderr);
    for (i = 0; i < count; i++) {
        if (i > 0) {
            fputc(' ', stderr);
        }
        fputs(words[i], stderr);
    }
    fputc('\n', stderr);
    fflush(stderr);
    arena_maybe_free(expanded_ps4);
    arena_maybe_free(out.items);
}

static bool noexec_allows_set_toggle(const char *source) {
    struct token_vec tokens;
    size_t i;
    bool allowed;

    tokens.items = NULL;
    tokens.len = 0;
    if (lexer_split_words(source, &tokens) != 0) {
        return false;
    }

    allowed = false;
    if (tokens.len == 0 || strcmp(tokens.items[0], "set") != 0) {
        lexer_free_tokens(&tokens);
        return false;
    }

    for (i = 1; i < tokens.len; i++) {
        const char *opt;
        size_t j;

        opt = tokens.items[i];
        if (strcmp(opt, "--") == 0) {
            break;
        }
        if (strcmp(opt, "+o") == 0) {
            if (i + 1 < tokens.len && strcmp(tokens.items[i + 1], "noexec") == 0) {
                allowed = true;
            }
            break;
        }
        if (strncmp(opt, "+o", 2) == 0 && strcmp(opt + 2, "noexec") == 0) {
            allowed = true;
            break;
        }
        if (opt[0] != '+' && opt[0] != '-') {
            break;
        }
        if (opt[0] != '+') {
            continue;
        }
        for (j = 1; opt[j] != '\0'; j++) {
            if (opt[j] == 'n') {
                allowed = true;
                break;
            }
        }
        if (allowed) {
            break;
        }
    }

    lexer_free_tokens(&tokens);
    return allowed;
}

static int word_vec_push(struct word_vec *words, char *word) {
    words->items = xrealloc(words->items, sizeof(*words->items) * (words->len + 1));
    words->items[words->len++] = word;
    return 0;
}

struct strip_heredoc_marker {
    char *delimiter;
    bool strip_tabs;
};

static void free_strip_heredoc_markers(struct strip_heredoc_marker *markers,
                                       size_t count) {
    size_t i;

    if (markers == NULL) {
        return;
    }
    for (i = 0; i < count; i++) {
        arena_maybe_free(markers[i].delimiter);
    }
    arena_maybe_free(markers);
}

static char *unquote_strip_heredoc_delimiter(const char *raw, size_t len) {
    size_t i;
    size_t out_len;
    char *out;

    out = arena_xmalloc(len + 1);
    out_len = 0;
    i = 0;
    while (i < len) {
        char ch;

        ch = raw[i];
        if (ch == '\\' && i + 1 < len) {
            out[out_len++] = raw[i + 1];
            i += 2;
            continue;
        }
        if (ch == '\'' || ch == '"') {
            char q;

            q = ch;
            i++;
            while (i < len && raw[i] != q) {
                if (q == '"' && raw[i] == '\\' && i + 1 < len) {
                    out[out_len++] = raw[i + 1];
                    i += 2;
                    continue;
                }
                out[out_len++] = raw[i];
                i++;
            }
            if (i < len && raw[i] == q) {
                i++;
            }
            continue;
        }
        out[out_len++] = ch;
        i++;
    }

    out[out_len] = '\0';
    return out;
}

static int push_strip_heredoc_marker(struct strip_heredoc_marker **markers,
                                     size_t *count, size_t *cap,
                                     const char *delimiter_raw,
                                     size_t delimiter_raw_len,
                                     bool strip_tabs) {
    struct strip_heredoc_marker *grown;
    char *unquoted;

    if (*count == *cap) {
        size_t new_cap;

        new_cap = *cap == 0 ? 4 : *cap * 2;
        grown = xrealloc(*markers, sizeof(**markers) * new_cap);
        *markers = grown;
        *cap = new_cap;
    }

    unquoted = unquote_strip_heredoc_delimiter(delimiter_raw, delimiter_raw_len);
    if (unquoted[0] == '\0') {
        arena_maybe_free(unquoted);
        return -1;
    }

    (*markers)[*count].delimiter = unquoted;
    (*markers)[*count].strip_tabs = strip_tabs;
    (*count)++;
    return 0;
}

static char *strip_comments(const char *src) {
    size_t i;
    size_t j;
    char quote;
    char prev;
    int param_depth;
    struct strip_heredoc_marker *markers;
    size_t marker_count;
    size_t marker_cap;
    size_t marker_idx;
    bool in_heredoc_body;
    char *out;

    out = arena_xmalloc(strlen(src) + 1);
    quote = '\0';
    prev = '\0';
    param_depth = 0;
    markers = NULL;
    marker_count = 0;
    marker_cap = 0;
    marker_idx = 0;
    in_heredoc_body = false;
    i = 0;
    j = 0;

    while (src[i] != '\0') {
        char ch;

        ch = src[i];

        if (in_heredoc_body) {
            size_t line_start;
            size_t line_end;
            size_t cmp_start;
            size_t delim_len;
            bool delimiter_match;

            line_start = i;
            while (src[i] != '\0' && src[i] != '\n') {
                i++;
            }
            line_end = i;

            cmp_start = line_start;
            if (markers[marker_idx].strip_tabs) {
                while (cmp_start < line_end && src[cmp_start] == '\t') {
                    cmp_start++;
                }
            }

            delim_len = strlen(markers[marker_idx].delimiter);
            delimiter_match = line_end - cmp_start == delim_len &&
                              memcmp(src + cmp_start, markers[marker_idx].delimiter,
                                     delim_len) == 0;

            memcpy(out + j, src + line_start, line_end - line_start);
            j += line_end - line_start;

            if (src[i] == '\n') {
                out[j++] = src[i++];
                prev = '\n';
            } else {
                prev = line_end > line_start ? src[line_end - 1] : prev;
            }

            if (delimiter_match) {
                marker_idx++;
                if (marker_idx >= marker_count) {
                    free_strip_heredoc_markers(markers, marker_count);
                    markers = NULL;
                    marker_count = 0;
                    marker_cap = 0;
                    marker_idx = 0;
                    in_heredoc_body = false;
                }
            }
            continue;
        }

        if (quote == '\0') {
            if (ch == '$' && src[i + 1] == '\'') {
                out[j++] = src[i++];
                out[j++] = src[i++];
                while (src[i] != '\0') {
                    out[j++] = src[i];
                    if (src[i] == '\\' && src[i + 1] != '\0') {
                        i++;
                        out[j++] = src[i];
                        i++;
                        continue;
                    }
                    if (src[i] == '\'') {
                        i++;
                        break;
                    }
                    i++;
                }
                prev = out[j - 1];
                continue;
            }
            if (ch == '$' && src[i + 1] == '{') {
                out[j++] = src[i++];
                out[j++] = src[i++];
                param_depth++;
                prev = out[j - 1];
                continue;
            }
            if (param_depth > 0) {
                if (ch == '{') {
                    param_depth++;
                } else if (ch == '}') {
                    param_depth--;
                }
            }
            if (ch == '\\' && src[i + 1] != '\0') {
                out[j++] = src[i++];
                out[j++] = src[i++];
                prev = out[j - 1];
                continue;
            }
            if (ch == '<' && src[i + 1] == '<') {
                size_t op_start;
                bool strip_tabs;
                size_t delim_start;
                size_t delim_end;
                size_t k;

                op_start = i;
                i += 2;
                strip_tabs = false;
                if (src[i] == '-') {
                    strip_tabs = true;
                    i++;
                }
                while (src[i] == ' ' || src[i] == '\t') {
                    i++;
                }
                delim_start = i;
                while (src[i] != '\0') {
                    if (isspace((unsigned char)src[i]) || src[i] == ';' ||
                        src[i] == '&' || src[i] == '|' || src[i] == '<' ||
                        src[i] == '>') {
                        break;
                    }
                    if (src[i] == '\\' && src[i + 1] != '\0') {
                        i += 2;
                        continue;
                    }
                    if (src[i] == '\'' || src[i] == '"') {
                        char q;

                        q = src[i++];
                        while (src[i] != '\0' && src[i] != q) {
                            if (q == '"' && src[i] == '\\' &&
                                src[i + 1] != '\0') {
                                i += 2;
                                continue;
                            }
                            i++;
                        }
                        if (src[i] == q) {
                            i++;
                        }
                        continue;
                    }
                    i++;
                }
                delim_end = i;

                if (delim_end > delim_start) {
                    push_strip_heredoc_marker(&markers, &marker_count, &marker_cap,
                                              src + delim_start,
                                              delim_end - delim_start, strip_tabs);
                }

                for (k = op_start; k < i; k++) {
                    out[j++] = src[k];
                }
                prev = out[j - 1];
                continue;
            }
            if (ch == '\'' || ch == '"') {
                quote = ch;
                out[j++] = src[i++];
                prev = ch;
                continue;
            }
            if (ch == '#' && param_depth == 0) {
                bool comment_start;

                comment_start = prev == '\0' || isspace((unsigned char)prev) ||
                                strchr("|&;()<>", prev) != NULL;
                if (comment_start) {
                    while (src[i] != '\0' && src[i] != '\n') {
                        i++;
                    }
                    continue;
                }
            }
        } else if (quote == '\'' && ch == '\'') {
            quote = '\0';
        } else if (quote == '"') {
            if (ch == '\\' && src[i + 1] != '\0') {
                out[j++] = src[i++];
                out[j++] = src[i++];
                prev = out[j - 1];
                continue;
            }
            if (ch == '"') {
                quote = '\0';
            }
        }

        out[j++] = src[i++];
        prev = out[j - 1];

        if (ch == '\n' && marker_count > 0 && marker_idx == 0) {
            in_heredoc_body = true;
        }
    }

    free_strip_heredoc_markers(markers, marker_count);
    out[j] = '\0';
    return out;
}

static bool has_unsupported_syntax(const char *source) {
    (void)source;
    return false;
}

static bool is_reserved_word_as_command(const char *word) {
    static const char *const reserved[] = {
        "if",   "then", "elif", "else", "fi",   "do",   "done",
        "case", "esac", "for",  "while", "until", "in",  "!",
    };
    size_t i;

    for (i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
        if (strcmp(word, reserved[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool is_name_start_char(char ch) {
    return isalpha((unsigned char)ch) || ch == '_';
}

static bool is_name_char(char ch) {
    return isalnum((unsigned char)ch) || ch == '_';
}

static bool is_assignment_word(const char *word) {
    size_t i;

    if (word[0] == '\0' || !is_name_start_char(word[0])) {
        return false;
    }

    i = 1;
    while (word[i] != '\0' && word[i] != '=') {
        if (!is_name_char(word[i])) {
            return false;
        }
        i++;
    }

    return word[i] == '=';
}

static size_t declaration_utility_prefix_len(const struct word_vec *raw_words,
                                             size_t assign_count) {
    if (assign_count >= raw_words->len) {
        return 0;
    }

    if (strcmp(raw_words->items[assign_count], "export") == 0 ||
        strcmp(raw_words->items[assign_count], "readonly") == 0) {
        return 1;
    }

    if (assign_count + 2 < raw_words->len &&
        strcmp(raw_words->items[assign_count], "command") == 0 &&
        strcmp(raw_words->items[assign_count + 1], "command") == 0 &&
        (strcmp(raw_words->items[assign_count + 2], "export") == 0 ||
         strcmp(raw_words->items[assign_count + 2], "readonly") == 0)) {
        return 3;
    }

    return 0;
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
    name = arena_xmalloc(nlen + 1);
    memcpy(name, word, nlen);
    name[nlen] = '\0';

    *name_out = name;
    *value_out = eq + 1;
    return 0;
}

static void free_positional_params(char **params, size_t count) {
    size_t i;

    for (i = 0; i < count; i++) {
        arena_maybe_free(params[i]);
    }
    arena_maybe_free(params);
}

static void positional_push(struct shell_state *state, char *const argv[], size_t argc,
                            struct positional_backup *backup) {
    size_t i;

    backup->params = state->positional_params;
    backup->count = state->positional_count;

    state->positional_params = NULL;
    state->positional_count = argc > 0 ? argc - 1 : 0;
    if (state->positional_count == 0) {
        return;
    }

    state->positional_params =
        arena_alloc_in(&state->arena_script,
                       sizeof(*state->positional_params) * state->positional_count);
    for (i = 0; i < state->positional_count; i++) {
        state->positional_params[i] =
            arena_strdup_in(&state->arena_script, argv[i + 1]);
    }
}

static void positional_pop(struct shell_state *state, const struct positional_backup *backup) {
    free_positional_params(state->positional_params, state->positional_count);
    state->positional_params = backup->params;
    state->positional_count = backup->count;
}

static int find_function_index(const struct shell_state *state, const char *name) {
    size_t i;

    for (i = 0; i < state->function_count; i++) {
        if (strcmp(state->functions[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int shell_set_function(struct shell_state *state, const char *name,
                              const char *body) {
    int idx;

    idx = find_function_index(state, name);
    if (idx >= 0) {
        arena_maybe_free(state->functions[idx].body);
        state->functions[idx].body = arena_strdup_in(&state->arena_perm, body);
        return 0;
    }

    state->functions = arena_realloc_in(
        &state->arena_perm, state->functions,
        sizeof(*state->functions) * (state->function_count + 1));
    state->functions[state->function_count].name =
        arena_strdup_in(&state->arena_perm, name);
    state->functions[state->function_count].body =
        arena_strdup_in(&state->arena_perm, body);
    state->function_count++;
    return 0;
}

static const char *shell_get_function(const struct shell_state *state, const char *name) {
    int idx;

    idx = find_function_index(state, name);
    if (idx < 0) {
        return NULL;
    }
    return state->functions[idx].body;
}

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
    while (i > 0 && (source[i - 1] == ' ' || source[i - 1] == '\t')) {
        i--;
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

static bool command_requires_program_runner(const char *source) {
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
            if (ch == ';' || ch == '\n') {
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


static bool parse_alt_parameter_command(const char *source, char **name_out,
                                        char **word_out) {
    size_t len;
    size_t i;
    size_t name_len;
    size_t word_start;

    *name_out = NULL;
    *word_out = NULL;

    len = strlen(source);
    if (len < 4 || source[0] != '$' || source[1] != '{' || source[len - 1] != '}') {
        return false;
    }

    i = 2;
    while (i + 2 < len) {
        if (source[i] == ':' && source[i + 1] == '+') {
            break;
        }
        i++;
    }
    if (i + 2 >= len || source[i] != ':' || source[i + 1] != '+') {
        return false;
    }

    name_len = i - 2;
    if (name_len == 0) {
        return false;
    }
    if (!is_name_start_char(source[2])) {
        return false;
    }
    for (i = 3; i < 2 + name_len; i++) {
        if (!is_name_char(source[i])) {
            return false;
        }
    }

    word_start = 2 + name_len + 2;
    *name_out = dup_trimmed_slice(source, 2, 2 + name_len);
    *word_out = dup_trimmed_slice(source, word_start, len - 1);
    return true;
}

static bool try_execute_alt_parameter_command(struct shell_state *state,
                                              const char *source,
                                              int *status_out) {
    char *name;
    char *word;
    const char *value;

    if (!parse_alt_parameter_command(source, &name, &word)) {
        return false;
    }

    value = getenv(name);
    if (value != NULL && value[0] != '\0' && word[0] != '\0') {
        *status_out = execute_program_text(state, word);
    } else {
        *status_out = 0;
    }

    arena_maybe_free(name);
    arena_maybe_free(word);
    return true;
}

static bool parse_function_definition(const char *source, char **name_out, char **body_out) {
    size_t i;
    size_t name_start;
    size_t name_end;
    size_t body_start;
    size_t body_end;

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

    /*
     * POSIX allows any compound command as a function body, not just brace
     * groups. Keep the full trailing command text as the function body.
     */
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

static bool looks_like_function_header_only(const char *source) {
    size_t i;

    i = 0;
    while (isspace((unsigned char)source[i])) {
        i++;
    }
    if (!is_name_start_char(source[i])) {
        return false;
    }
    i++;
    while (is_name_char(source[i])) {
        i++;
    }
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
    return source[i] == '\0';
}

static bool program_contains_quoted_heredoc(const char *source) {
    size_t i;
    char quote;

    i = 0;
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
            if (ch == '<' && source[i + 1] == '<') {
                size_t j;
                bool saw_quote_or_backslash;

                j = i + 2;
                if (source[j] == '-') {
                    j++;
                }
                while (source[j] == ' ' || source[j] == '\t') {
                    j++;
                }
                saw_quote_or_backslash = false;
                while (source[j] != '\0' && !isspace((unsigned char)source[j]) &&
                       source[j] != ';' && source[j] != '&' && source[j] != '|' &&
                       source[j] != '<' && source[j] != '>') {
                    if (source[j] == '\'' || source[j] == '"' || source[j] == '\\') {
                        saw_quote_or_backslash = true;
                    }
                    if (source[j] == '\\' && source[j + 1] != '\0') {
                        j += 2;
                        continue;
                    }
                    if (source[j] == '\'' || source[j] == '"') {
                        char q;

                        q = source[j++];
                        while (source[j] != '\0' && source[j] != q) {
                            if (q == '"' && source[j] == '\\' &&
                                source[j + 1] != '\0') {
                                j += 2;
                                continue;
                            }
                            j++;
                        }
                        if (source[j] == q) {
                            j++;
                        }
                        continue;
                    }
                    j++;
                }
                if (saw_quote_or_backslash) {
                    return true;
                }
                i = j;
                continue;
            }
        } else if (quote == '\'' && ch == '\'') {
            quote = '\0';
        } else if (quote == '"') {
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

static bool has_pending_flow_control(const struct shell_state *state) {
    return state->break_levels > 0 || state->continue_levels > 0 ||
           state->return_requested;
}

static bool ignore_helper_function_declaration(const char *source) {
    (void)source;
    return false;
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
            if (ch == '$' && token[i + 1] == '{') {
                size_t j;
                int depth;
                char inner_quote;

                j = i + 2;
                depth = 1;
                inner_quote = '\0';
                while (token[j] != '\0' && depth > 0) {
                    char inner;

                    inner = token[j];
                    if (inner_quote == '\0') {
                        if (inner == '\\' && token[j + 1] != '\0') {
                            j += 2;
                            continue;
                        }
                        if (inner == '\'' || inner == '"') {
                            inner_quote = inner;
                            j++;
                            continue;
                        }
                        if (inner == '{') {
                            depth++;
                        } else if (inner == '}') {
                            depth--;
                            if (depth == 0) {
                                i = j;
                                break;
                            }
                        }
                        j++;
                        continue;
                    }
                    if (inner_quote == '\'' && inner == '\'') {
                        inner_quote = '\0';
                        j++;
                        continue;
                    }
                    if (inner_quote == '"') {
                        if (inner == '\\' && token[j + 1] != '\0') {
                            j += 2;
                            continue;
                        }
                        if (inner == '"') {
                            inner_quote = '\0';
                        }
                    }
                    j++;
                }
                if (depth == 0) {
                    continue;
                }
            }
            if (ch == '`') {
                i++;
                while (token[i] != '\0') {
                    if (token[i] == '\\' && token[i + 1] != '\0') {
                        i += 2;
                        continue;
                    }
                    if (token[i] == '`') {
                        break;
                    }
                    i++;
                }
                if (token[i] == '\0') {
                    return -1;
                }
                continue;
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

static int collect_words_and_redirs(const struct token_vec *expanded, struct word_vec *words,
                                    struct redir_vec *redirs) {
    size_t i;

    words->items = NULL;
    words->len = 0;
    redirs->items = NULL;
    redirs->len = 0;

    for (i = 0; i < expanded->len; i++) {
        struct redir_spec spec;
        bool needs_word;
        int pr;

        pr = parse_redir_token(expanded->items[i], &spec, &needs_word);
        if (pr < 0) {
            return -1;
        }
        if (pr == 0) {
            size_t op_pos;

            if (find_redir_operator_pos(expanded->items[i], &op_pos) == 0 &&
                op_pos > 0) {
                char *prefix;
                const char *redir_text;

                prefix = arena_xmalloc(op_pos + 1);
                memcpy(prefix, expanded->items[i], op_pos);
                prefix[op_pos] = '\0';
                word_vec_push(words, prefix);

                redir_text = expanded->items[i] + op_pos;
                pr = parse_redir_token(redir_text, &spec, &needs_word);
                if (pr < 0) {
                    return -1;
                }
                if (pr == 0) {
                    word_vec_push(words, expanded->items[i]);
                    continue;
                }
            } else {
                word_vec_push(words, expanded->items[i]);
                continue;
            }
        }

        if (needs_word) {
            i++;
            if (i >= expanded->len) {
                posish_error_idf(POSERR_MISSING_REDIRECTION_OPERAND);
                return -1;
            }

            spec.path = arena_xstrdup(expanded->items[i]);
        }

        redir_vec_push(redirs, &spec);
    }

    return 0;
}

static int expand_redirection_operands(struct shell_state *state,
                                       struct redir_vec *redirs,
                                       bool *saw_cmdsub_out,
                                       int *last_cmdsub_status_out) {
    size_t i;
    bool saw_cmdsub;
    int last_cmdsub_status;
    struct token_vec in_vec;
    struct token_vec out_vec;

    saw_cmdsub = false;
    last_cmdsub_status = 0;

    for (i = 0; i < redirs->len; i++) {
        char *one_word;

        if (redirs->items[i].path == NULL) {
            continue;
        }

        one_word = redirs->items[i].path;
        in_vec.items = &one_word;
        in_vec.len = 1;
        out_vec.items = NULL;
        out_vec.len = 0;

        if (expand_words(&in_vec, &out_vec, state, false) != 0) {
            return 2;
        }
        if (state->cmdsub_performed) {
            saw_cmdsub = true;
            last_cmdsub_status = state->last_cmdsub_status;
        }
        if (out_vec.len != 1) {
            size_t j;
            for (j = 0; j < out_vec.len; j++) {
                arena_maybe_free(out_vec.items[j]);
            }
            arena_maybe_free(out_vec.items);
            posish_error_idf(POSERR_AMBIGUOUS_REDIRECTION);
            return 1;
        }

        arena_maybe_free(redirs->items[i].path);
        redirs->items[i].path = out_vec.items[0];
        arena_maybe_free(out_vec.items);

        if (redirs->items[i].kind == REDIR_DUP_IN ||
            redirs->items[i].kind == REDIR_DUP_OUT) {
            if (parse_dup_operand(redirs->items[i].path, &redirs->items[i]) !=
                0) {
                posish_error_idf(POSERR_INVALID_FD_REDIRECTION,
                                 redirs->items[i].path);
                return 1;
            }
            arena_maybe_free(redirs->items[i].path);
            redirs->items[i].path = NULL;
        }
    }

    if (saw_cmdsub_out != NULL) {
        *saw_cmdsub_out = saw_cmdsub;
    }
    if (last_cmdsub_status_out != NULL) {
        *last_cmdsub_status_out = last_cmdsub_status;
    }
    return 0;
}

static int apply_persistent_assignments(struct shell_state *state,
                                        char *const words[], size_t count) {
    size_t i;

    for (i = 0; i < count; i++) {
        char *name;
        const char *value;

        if (split_assignment(words[i], &name, &value) != 0) {
            continue;
        }

        if (vars_set_assignment(state, name, value, true) != 0) {
            if (!state->interactive) {
                state->should_exit = true;
                state->exit_status = 1;
            }
            arena_maybe_free(name);
            return 1;
        }
        arena_maybe_free(name);
    }

    return 0;
}

static void restore_temporary_assignments(struct shell_state *state,
                                          struct env_restore_vec *restore) {
    size_t i;

    for (i = restore->len; i > 0; i--) {
        struct env_restore *r;

        r = &restore->items[i - 1];
        if (r->existed) {
            if (r->was_unexported) {
                if (vars_set_with_mode(state, r->name, r->old_value, false, false) !=
                    0) {
                    perror("setenv");
                }
            } else if (setenv(r->name, r->old_value, 1) != 0) {
                perror("setenv");
            }
        } else {
            if (unsetenv(r->name) != 0) {
                perror("unsetenv");
            }
        }
    }
}

static int apply_temporary_assignments(struct shell_state *state,
                                       char *const words[], size_t count,
                                       struct env_restore_vec *restore) {
    size_t i;

    restore->items = NULL;
    restore->len = 0;

    for (i = 0; i < count; i++) {
        struct env_restore r;
        char *name;
        const char *value;
        const char *old;

        if (split_assignment(words[i], &name, &value) != 0) {
            continue;
        }

        r.name = name;
        r.old_value = NULL;
        r.existed = false;
        r.was_unexported = false;

        old = getenv(name);
        if (old != NULL) {
            r.old_value = arena_xstrdup(old);
            r.existed = true;
            r.was_unexported = vars_is_unexported(state, name);
        }

        if (vars_set(state, name, value, true) != 0) {
            if (!state->interactive) {
                state->should_exit = true;
                state->exit_status = 1;
            }
            arena_maybe_free(r.name);
            arena_maybe_free(r.old_value);
            restore_temporary_assignments(state, restore);
            env_restore_vec_free(restore);
            return 1;
        }

        restore->items = xrealloc(restore->items, sizeof(*restore->items) * (restore->len + 1));
        restore->items[restore->len++] = r;
    }

    return 0;
}

static bool is_exec_without_command(char *const argv[]) {
    size_t i;

    if (strcmp(argv[0], "exec") != 0) {
        return false;
    }

    i = 1;
    if (argv[i] != NULL && strcmp(argv[i], "--") == 0) {
        i++;
    }

    return argv[i] == NULL;
}

static bool is_command_exec_without_command(char *const argv[]) {
    size_t i;
    bool opt_v;
    bool opt_V;

    if (strcmp(argv[0], "command") != 0) {
        return false;
    }

    i = 1;
    opt_v = false;
    opt_V = false;
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
                continue;
            }
            return false;
        }
        i++;
    }

    if (opt_v || opt_V) {
        return false;
    }
    if (argv[i] == NULL || strcmp(argv[i], "exec") != 0) {
        return false;
    }
    i++;
    if (argv[i] != NULL && strcmp(argv[i], "--") == 0) {
        i++;
    }
    return argv[i] == NULL;
}

static int run_external_argv(struct shell_state *state, char *const argv[],
                             const struct redir_vec *redirs) {
    int status;
    pid_t pid;

    trace_log(POSISH_TRACE_SIGNALS, "spawn external argv0=%s", argv[0]);
    pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        if (state->monitor_mode) {
            /*
             * In monitor mode, place the command in its own process group so
             * stop signals are not suppressed for orphaned groups.
             */
            (void)setpgid(0, 0);
        }

        (void)setenv("POSISH_PARENT_INTERACTIVE", state->interactive ? "1" : "0",
                     1);
        exec_prepare_signals_for_exec_child(state);
        vars_apply_unexported_in_child(state);

        if (apply_redirections(redirs, false, state->noclobber, NULL) != 0) {
            _exit(1);
        }

        execvp(argv[0], argv);
        perror(argv[0]);
        _exit(127);
    }

    if (state->monitor_mode) {
        (void)setpgid(pid, pid);
    }

    trace_log(POSISH_TRACE_SIGNALS, "waiting external pid=%ld", (long)pid);

    for (;;) {
        if (waitpid(pid, &status, WUNTRACED) < 0) {
            if (errno == EINTR) {
                shell_run_pending_traps(state);
                continue;
            }
            perror("waitpid");
            return 1;
        }
        break;
    }

    if (WIFEXITED(status)) {
        trace_log(POSISH_TRACE_SIGNALS, "external pid=%ld exited=%d", (long)pid,
                  WEXITSTATUS(status));
        return WEXITSTATUS(status);
    }
    if (WIFSTOPPED(status)) {
        jobs_track_job(pid, &pid, 1, pid, argv[0], true);
        jobs_note_process_status(pid, status);
        trace_log(POSISH_TRACE_SIGNALS, "external pid=%ld stopped sig=%d",
                  (long)pid, WSTOPSIG(status));
        return shell_status_from_wait_status(status);
    }
    if (WIFSIGNALED(status)) {
        trace_log(POSISH_TRACE_SIGNALS, "external pid=%ld signaled sig=%d",
                  (long)pid, WTERMSIG(status));
        return shell_status_from_wait_status(status);
    }
    return 1;
}

static bool path_resolves_command(const char *name) {
    const char *path;
    const char *p;

    if (name == NULL || name[0] == '\0') {
        return false;
    }
    if (strchr(name, '/') != NULL) {
        return access(name, X_OK) == 0;
    }

    path = getenv("PATH");
    if (path == NULL || path[0] == '\0') {
        return false;
    }

    p = path;
    while (1) {
        const char *end;
        size_t dlen;
        const char *dir;
        char *candidate;
        bool found;

        end = strchr(p, ':');
        if (end == NULL) {
            end = p + strlen(p);
        }
        dlen = (size_t)(end - p);
        dir = dlen == 0 ? "." : p;

        candidate = arena_xmalloc((dlen == 0 ? 1 : dlen) + 1 + strlen(name) + 1);
        if (dlen == 0) {
            strcpy(candidate, ".");
        } else {
            memcpy(candidate, dir, dlen);
            candidate[dlen] = '\0';
        }
        strcat(candidate, "/");
        strcat(candidate, name);

        found = access(candidate, X_OK) == 0;
        arena_maybe_free(candidate);
        if (found) {
            return true;
        }
        if (*end == '\0') {
            break;
        }
        p = end + 1;
    }

    return false;
}

static int parse_redirections_from_source(const char *source, struct shell_state *state,
                                          struct redir_vec *redirs) {
    struct token_vec lexed;
    struct token_vec expanded;
    struct word_vec words;
    int rc;

    redirs->items = NULL;
    redirs->len = 0;

    if (source[0] == '\0') {
        return 0;
    }

    if (lexer_split_words(source, &lexed) != 0) {
        return -1;
    }
    if (expand_words(&lexed, &expanded, state, false) != 0) {
        lexer_free_tokens(&lexed);
        return -1;
    }
    lexer_free_tokens(&lexed);

    rc = collect_words_and_redirs(&expanded, &words, redirs);
    if (rc != 0) {
        lexer_free_tokens(&expanded);
        return -1;
    }
    if (expand_redirection_operands(state, redirs, NULL, NULL) != 0) {
        word_vec_free(&words);
        redir_vec_free(redirs);
        lexer_free_tokens(&expanded);
        return -1;
    }

    if (words.len != 0) {
        posish_error_idf(POSERR_UNSUPPORTED_TOKENS_AFTER_GROUP);
        word_vec_free(&words);
        redir_vec_free(redirs);
        lexer_free_tokens(&expanded);
        return -1;
    }

    word_vec_free(&words);
    lexer_free_tokens(&expanded);
    return 0;
}

static int execute_simple_command(struct shell_state *state, const char *source,
                                  bool allow_builtin) {
    struct token_vec lexed;
    struct word_vec raw_words;
    struct token_vec expanded;
    struct token_vec assign_expanded;
    struct token_vec cmd_expanded;
    struct word_vec words;
    struct redir_vec redirs;
    struct env_restore_vec temp_env;
    struct fd_backup_vec fd_backups;
    struct fd_backup_vec pre_expand_backups;
    char **argv;
    size_t i;
    size_t assign_count;
    size_t argc;
    int status;
    bool handled;
    bool have_temp_env;
    bool special_name;
    bool assignment_special;
    bool persist_builtin_redirs;
    const char *function_body;
    struct positional_backup positional_backup;
    bool saw_cmdsub;
    int last_cmdsub_status;
    bool pre_expand_redirs;
    bool redirs_only_command;
    struct token_vec in_vec;

    lexed.items = NULL;
    lexed.len = 0;
    raw_words.items = NULL;
    raw_words.len = 0;
    expanded.items = NULL;
    expanded.len = 0;
    assign_expanded.items = NULL;
    assign_expanded.len = 0;
    cmd_expanded.items = NULL;
    cmd_expanded.len = 0;
    words.items = NULL;
    words.len = 0;
    redirs.items = NULL;
    redirs.len = 0;
    argv = NULL;
    status = 0;
    handled = false;
    have_temp_env = false;
    function_body = NULL;
    saw_cmdsub = false;
    last_cmdsub_status = 0;
    pre_expand_redirs = false;
    redirs_only_command = false;
    assignment_special = false;

    if (has_unsupported_syntax(source)) {
        posish_error_idf(POSERR_COMPLEX_SYNTAX_UNIMPLEMENTED);
        return 2;
    }

    if (lexer_split_words_at(state->current_source_name, source,
                             state->current_source_base_line, &lexed) != 0) {
        return 2;
    }

    if (collect_words_and_redirs(&lexed, &raw_words, &redirs) != 0) {
        status = 2;
        goto done;
    }
    redirs_only_command = raw_words.len == 0;

    assign_count = 0;
    while (assign_count < raw_words.len && is_assignment_word(raw_words.items[assign_count])) {
        assign_count++;
    }

    in_vec.items = raw_words.items + assign_count;
    in_vec.len = raw_words.len - assign_count;
    if (in_vec.len > 0 && is_reserved_word_as_command(in_vec.items[0])) {
        posish_error_idf(POSERR_UNEXPECTED_TOKEN, in_vec.items[0]);
        if (!state->interactive) {
            state->should_exit = true;
            state->exit_status = 2;
        }
        status = 2;
        goto done;
    }
    if (in_vec.len > 0) {
        size_t decl_prefix_len;
        size_t wi;

        decl_prefix_len = declaration_utility_prefix_len(&raw_words, assign_count);
        for (wi = 0; wi < in_vec.len; wi++) {
            struct token_vec one_in;
            struct token_vec one_out;
            bool split_fields;
            size_t oi;

            one_in.items = &in_vec.items[wi];
            one_in.len = 1;
            one_out.items = NULL;
            one_out.len = 0;

            split_fields = true;
            if (decl_prefix_len > 0 && wi >= decl_prefix_len &&
                is_assignment_word(in_vec.items[wi])) {
                /*
                 * Declaration utilities treat assignment operands as a single
                 * word after expansion (no field splitting/pathname expansion).
                 */
                split_fields = false;
            }

            if (expand_words(&one_in, &one_out, state, split_fields) != 0) {
                lexer_free_tokens(&cmd_expanded);
                status = 2;
                goto done;
            }
            if (state->cmdsub_performed) {
                saw_cmdsub = true;
                last_cmdsub_status = state->last_cmdsub_status;
            }
            if (one_out.len > 0) {
                cmd_expanded.items = arena_xrealloc(
                    cmd_expanded.items,
                    sizeof(*cmd_expanded.items) * (cmd_expanded.len + one_out.len));
                for (oi = 0; oi < one_out.len; oi++) {
                    cmd_expanded.items[cmd_expanded.len++] = one_out.items[oi];
                }
            }
            arena_maybe_free(one_out.items);
        }
    }

    special_name = allow_builtin && cmd_expanded.len > 0 &&
                   cmd_expanded.items[0][0] != '\0' &&
                   builtin_is_special_name(cmd_expanded.items[0]);
    assignment_special =
        special_name && strcmp(cmd_expanded.items[0], "command") != 0;

    if (!redirs_only_command) {
        bool redir_saw_cmdsub;
        int redir_last_cmdsub_status;

        redir_saw_cmdsub = false;
        redir_last_cmdsub_status = 0;
        status = expand_redirection_operands(state, &redirs, &redir_saw_cmdsub,
                                             &redir_last_cmdsub_status);
        if (redir_saw_cmdsub) {
            saw_cmdsub = true;
            last_cmdsub_status = redir_last_cmdsub_status;
        }
        if (status != 0) {
            goto done;
        }
    }

    if (assign_count > 0 && cmd_expanded.len > 0 && !assignment_special) {
        /*
         * For regular commands, redirections are established before assignment
         * expansion so command substitutions in assignments observe them.
         */
        pre_expand_backups.items = NULL;
        pre_expand_backups.len = 0;
        if (apply_redirections(&redirs, true, state->noclobber,
                               &pre_expand_backups) != 0) {
            status = 1;
            goto done;
        }
        pre_expand_redirs = true;
    }

    in_vec.items = raw_words.items;
    in_vec.len = assign_count;
    if (in_vec.len > 0) {
        if (expand_words(&in_vec, &assign_expanded, state, false) != 0) {
            status = 2;
            goto done;
        }
        if (state->cmdsub_performed) {
            saw_cmdsub = true;
            last_cmdsub_status = state->last_cmdsub_status;
        }
    }

    if (pre_expand_redirs) {
        fd_backup_restore(&pre_expand_backups);
        pre_expand_redirs = false;
    }

    state->cmdsub_performed = saw_cmdsub;
    state->last_cmdsub_status = saw_cmdsub ? last_cmdsub_status : 0;

    expanded.len = assign_expanded.len + cmd_expanded.len;
    if (expanded.len > 0) {
        expanded.items = arena_xmalloc(sizeof(*expanded.items) * expanded.len);
        for (i = 0; i < assign_expanded.len; i++) {
            expanded.items[i] = assign_expanded.items[i];
        }
        for (i = 0; i < cmd_expanded.len; i++) {
            expanded.items[assign_expanded.len + i] = cmd_expanded.items[i];
        }
    }
    arena_maybe_free(assign_expanded.items);
    assign_expanded.items = NULL;
    assign_expanded.len = 0;
    arena_maybe_free(cmd_expanded.items);
    cmd_expanded.items = NULL;
    cmd_expanded.len = 0;

    if (expanded.len > 0) {
        words.items = arena_xmalloc(sizeof(*words.items) * expanded.len);
        for (i = 0; i < expanded.len; i++) {
            words.items[i] = expanded.items[i];
        }
        words.len = expanded.len;
    }

    word_vec_free(&raw_words);
    lexer_free_tokens(&lexed);

    if (words.len == 0) {
        if (redirs_only_command) {
            pid_t pid;
            int wstatus;

            pid = fork();
            if (pid < 0) {
                perror("fork");
                redir_vec_free(&redirs);
                word_vec_free(&words);
                lexer_free_tokens(&expanded);
                return 1;
            }
            if (pid == 0) {
                struct shell_state local_state;
                bool redir_saw_cmdsub;
                int redir_last_cmdsub_status;
                int st;

                local_state = *state;
                arena_init(&local_state.arena_perm, state->arena_perm.default_block_size);
                arena_init(&local_state.arena_script,
                           state->arena_script.default_block_size);
                arena_init(&local_state.arena_cmd, state->arena_cmd.default_block_size);
                arena_set_current(&local_state.arena_perm);
                local_state.should_exit = false;
                local_state.exit_status = 0;
                local_state.running_signal_trap = false;
                local_state.running_exit_trap = false;
                local_state.main_context = false;
                redir_saw_cmdsub = false;
                redir_last_cmdsub_status = 0;

                st = expand_redirection_operands(&local_state, &redirs,
                                                 &redir_saw_cmdsub,
                                                 &redir_last_cmdsub_status);
                if (st != 0) {
                    _exit(st);
                }
                if (redir_saw_cmdsub) {
                    local_state.cmdsub_performed = true;
                    local_state.last_cmdsub_status = redir_last_cmdsub_status;
                }
                if (apply_redirections(&redirs, false, local_state.noclobber,
                                       NULL) != 0) {
                    _exit(1);
                }
                st = local_state.cmdsub_performed ? local_state.last_cmdsub_status
                                                  : 0;
                _exit(st);
            }

            for (;;) {
                if (waitpid(pid, &wstatus, 0) < 0) {
                    if (errno == EINTR) {
                        shell_run_pending_traps(state);
                        continue;
                    }
                    perror("waitpid");
                    redir_vec_free(&redirs);
                    word_vec_free(&words);
                    lexer_free_tokens(&expanded);
                    return 1;
                }
                break;
            }

            redir_vec_free(&redirs);
            word_vec_free(&words);
            lexer_free_tokens(&expanded);
            if (WIFEXITED(wstatus)) {
                return WEXITSTATUS(wstatus);
            }
            if (WIFSIGNALED(wstatus)) {
                return shell_status_from_wait_status(wstatus);
            }
            return 1;
        }

        fd_backups.items = NULL;
        fd_backups.len = 0;
        if (apply_redirections(&redirs, true, state->noclobber, &fd_backups) !=
            0) {
            fd_backup_restore(&fd_backups);
            redir_vec_free(&redirs);
            word_vec_free(&words);
            lexer_free_tokens(&expanded);
            return 1;
        }
        fflush(NULL);
        fd_backup_restore(&fd_backups);
        redir_vec_free(&redirs);
        word_vec_free(&words);
        lexer_free_tokens(&expanded);
        if (state->cmdsub_performed) {
            return state->last_cmdsub_status;
        }
        return 0;
    }

    trace_simple_words(state, words.items, words.len);

    assign_count = 0;
    while (assign_count < words.len && is_assignment_word(words.items[assign_count])) {
        assign_count++;
    }

    if (assign_count == words.len) {
        fd_backups.items = NULL;
        fd_backups.len = 0;

        status = apply_persistent_assignments(state, words.items, assign_count);
        if (status == 0) {
            if (apply_redirections(&redirs, true, state->noclobber,
                                   &fd_backups) != 0) {
                status = 1;
            }
        }
        fflush(NULL);
        fd_backup_restore(&fd_backups);

        redir_vec_free(&redirs);
        word_vec_free(&words);
        lexer_free_tokens(&expanded);
        if (status == 0 && state->cmdsub_performed) {
            return state->last_cmdsub_status;
        }
        return status;
    }

    argc = words.len - assign_count;
    argv = arena_xmalloc(sizeof(*argv) * (argc + 1));
    for (i = 0; i < argc; i++) {
        argv[i] = words.items[assign_count + i];
    }
    argv[argc] = NULL;
    if (argc == 1 && argv[0][0] == '\0') {
        fd_backups.items = NULL;
        fd_backups.len = 0;
        status = apply_persistent_assignments(state, words.items, assign_count);
        if (status == 0) {
            if (apply_redirections(&redirs, true, state->noclobber,
                                   &fd_backups) != 0) {
                status = 1;
            }
        }
        fflush(NULL);
        fd_backup_restore(&fd_backups);
        arena_maybe_free(argv);
        redir_vec_free(&redirs);
        word_vec_free(&words);
        lexer_free_tokens(&expanded);
        if (status == 0 && state->cmdsub_performed) {
            return state->last_cmdsub_status;
        }
        return status;
    }

    special_name = allow_builtin && builtin_is_special_name(argv[0]);
    function_body = allow_builtin && !special_name ?
                        shell_get_function(state, argv[0]) :
                        NULL;
    assignment_special = special_name && strcmp(argv[0], "command") != 0;
    persist_builtin_redirs =
        (special_name && is_exec_without_command(argv)) ||
        (allow_builtin && is_command_exec_without_command(argv));

    if (assign_count > 0) {
        if (assignment_special) {
            status = apply_persistent_assignments(state, words.items, assign_count);
            if (status != 0) {
                goto done;
            }
        } else {
            if (apply_temporary_assignments(state, words.items, assign_count,
                                            &temp_env) != 0) {
                status = 1;
                goto done;
            }
            have_temp_env = true;
        }
    }

    fd_backups.items = NULL;
    fd_backups.len = 0;

    if (allow_builtin) {
        bool run_in_shell;
        bool builtin_available;

        builtin_available = builtin_is_name(argv[0]) &&
                            (!builtin_is_substitutive_name(argv[0]) ||
                             path_resolves_command(argv[0]));
        run_in_shell = function_body != NULL || builtin_available;
        if (persist_builtin_redirs) {
            if (apply_redirections(&redirs, false, state->noclobber, NULL) !=
                0) {
                status = 1;
                if (special_name && !state->interactive) {
                    state->should_exit = true;
                    state->exit_status = status;
                }
                goto done;
            }

            status = builtin_dispatch(state, argv, &handled);
            if (!handled) {
                status = run_external_argv(state, argv, &redirs);
            }
        } else if (run_in_shell) {
            if (apply_redirections(&redirs, true, state->noclobber,
                                   &fd_backups) != 0) {
                status = 1;
                if (special_name && !state->interactive) {
                    state->should_exit = true;
                    state->exit_status = status;
                }
                fd_backup_restore(&fd_backups);
                goto done;
            }

            if (function_body != NULL) {
                /* Functions run in the current shell with temporary $1..$n. */
                positional_push(state, argv, argc, &positional_backup);
                state->function_depth++;
                status = execute_program_text(state, function_body);
                state->function_depth--;
                positional_pop(state, &positional_backup);
                if (state->return_requested) {
                    status = state->return_status;
                    state->return_requested = false;
                }
                handled = true;
            } else {
                status = builtin_dispatch(state, argv, &handled);
            }
            fflush(NULL);
            fd_backup_restore(&fd_backups);

            if (!handled) {
                status = run_external_argv(state, argv, &redirs);
            }
        } else {
            /*
             * External utilities apply redirections in the child only.
             * Doing it in the parent can trigger FIFO side effects before
             * exec (and regress async semantics).
             */
            status = run_external_argv(state, argv, &redirs);
        }
    } else {
        status = run_external_argv(state, argv, &redirs);
    }

done:
    if (pre_expand_redirs) {
        fd_backup_restore(&pre_expand_backups);
    }
    if (have_temp_env) {
        restore_temporary_assignments(state, &temp_env);
        env_restore_vec_free(&temp_env);
    }

    if (status == 0 && state->cmdsub_performed && argc == 1 &&
        argv[0][0] == '\0') {
        status = state->last_cmdsub_status;
    }

    arena_maybe_free(argv);
    redir_vec_free(&redirs);
    word_vec_free(&words);
    word_vec_free(&raw_words);
    lexer_free_tokens(&expanded);
    lexer_free_tokens(&assign_expanded);
    lexer_free_tokens(&cmd_expanded);
    lexer_free_tokens(&lexed);
    return status;
}

static bool unwrap_subshell_group(const char *source, char **inner_out,
                                  char **redir_suffix_out) {
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

    *inner_out = dup_trimmed_slice(source, 1, close_pos);
    *redir_suffix_out = dup_trimmed_slice(source, close_pos + 1, len);
    return true;
}

static bool unwrap_brace_group(const char *source, char **inner_out, char **redir_suffix_out) {
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

    *inner_out = dup_trimmed_slice(source, 1, close_pos);
    *redir_suffix_out = dup_trimmed_slice(source, close_pos + 1, len);
    return true;
}

static int run_subshell_command(struct shell_state *parent_state,
                                const char *source) {
    pid_t pid;
    int status;

    trace_log(POSISH_TRACE_SIGNALS, "spawn subshell");
    pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        struct shell_state local_state;
        int st;

        if (parent_state->monitor_mode) {
            (void)setpgid(0, 0);
        }

        local_state = *parent_state;
        arena_init(&local_state.arena_perm,
                   parent_state->arena_perm.default_block_size);
        arena_init(&local_state.arena_script,
                   parent_state->arena_script.default_block_size);
        arena_init(&local_state.arena_cmd,
                   parent_state->arena_cmd.default_block_size);
        arena_set_current(&local_state.arena_perm);
        local_state.should_exit = false;
        local_state.exit_status = 0;
        local_state.running_signal_trap = false;
        local_state.running_exit_trap = false;
        local_state.main_context = false;
        signals_reset_traps_for_child(&local_state);
        signals_reset_exit_trap_for_child(&local_state);

        st = execute_program_text(&local_state, source);
        shell_run_pending_traps(&local_state);
        shell_run_exit_trap(&local_state);
        if (local_state.should_exit) {
            st = local_state.exit_status;
        }
        fflush(NULL);
        exit_shell_child_status(st);
    }

    if (parent_state->monitor_mode) {
        (void)setpgid(pid, pid);
    }

    for (;;) {
        if (waitpid(pid, &status, WUNTRACED) < 0) {
            if (errno == EINTR) {
                /*
                 * Defer trap execution until the foreground subshell finishes.
                 * This keeps trap side effects ordered after child output.
                 */
                continue;
            }
            perror("waitpid");
            return 1;
        }
        break;
    }

    if (WIFEXITED(status)) {
        trace_log(POSISH_TRACE_SIGNALS, "subshell pid=%ld exited=%d", (long)pid,
                  WEXITSTATUS(status));
        return WEXITSTATUS(status);
    }
    if (WIFSTOPPED(status)) {
        jobs_track_job(pid, &pid, 1, pid, source, true);
        jobs_note_process_status(pid, status);
        trace_log(POSISH_TRACE_SIGNALS, "subshell pid=%ld stopped sig=%d",
                  (long)pid, WSTOPSIG(status));
        return shell_status_from_wait_status(status);
    }
    if (WIFSIGNALED(status)) {
        trace_log(POSISH_TRACE_SIGNALS, "subshell pid=%ld signaled sig=%d",
                  (long)pid, WTERMSIG(status));
        return shell_status_from_wait_status(status);
    }
    return 1;
}

static int run_subshell_group_command(struct shell_state *state,
                                      const char *body,
                                      const char *redir_suffix) {
    struct redir_vec redirs;
    struct fd_backup_vec backups;
    int status;

    if (parse_redirections_from_source(redir_suffix, state, &redirs) != 0) {
        return 2;
    }

    backups.items = NULL;
    backups.len = 0;
    if (apply_redirections(&redirs, true, state->noclobber, &backups) != 0) {
        fd_backup_restore(&backups);
        redir_vec_free(&redirs);
        return 1;
    }

    status = run_subshell_command(state, body);
    fd_backup_restore(&backups);
    redir_vec_free(&redirs);
    return status;
}

static int run_async_list(struct shell_state *state, const char *source) {
    pid_t pid;

    trace_log(POSISH_TRACE_SIGNALS, "spawn async list source=%s", source);
    pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        struct shell_state local_state;
        int st;
        int nullfd;

        if (state->monitor_mode) {
            (void)setpgid(0, 0);
        }

        local_state = *state;
        arena_init(&local_state.arena_perm, state->arena_perm.default_block_size);
        arena_init(&local_state.arena_script, state->arena_script.default_block_size);
        arena_init(&local_state.arena_cmd, state->arena_cmd.default_block_size);
        arena_set_current(&local_state.arena_perm);
        local_state.should_exit = false;
        local_state.exit_status = 0;
        local_state.running_signal_trap = false;
        local_state.running_exit_trap = false;
        local_state.in_async_context = true;
        local_state.main_context = false;
        signals_reset_traps_for_child(&local_state);
        signals_reset_exit_trap_for_child(&local_state);

        /*
         * With job control disabled (+m), asynchronous lists run with INT/QUIT
         * ignored until explicitly changed by a trap inside that async context.
         */
#ifdef SIGINT
        if (!state->monitor_mode) {
            (void)signals_set_ignored(SIGINT);
            local_state.signal_cleared[SIGINT] = false;
        }
#endif
#ifdef SIGQUIT
        if (!state->monitor_mode) {
            (void)signals_set_ignored(SIGQUIT);
            local_state.signal_cleared[SIGQUIT] = false;
        }
#endif
        /*
         * In non-monitor mode (+m), asynchronous lists read from /dev/null.
         * This intentionally overrides inherited stdin redirections.
         */
        if (!state->monitor_mode) {
            nullfd = open("/dev/null", O_RDONLY);
            if (nullfd >= 0) {
                if (dup2(nullfd, STDIN_FILENO) < 0) {
                    perror("dup2");
                    _exit(1);
                }
                if (nullfd != STDIN_FILENO) {
                    close(nullfd);
                }
            }
        }

        /*
         * Async control operator applies to a full command list, not just a
         * single and-or segment.
         */
        st = execute_program_text(&local_state, source);
        shell_run_pending_traps(&local_state);
        shell_run_exit_trap(&local_state);
        if (local_state.should_exit) {
            st = local_state.exit_status;
        }
        fflush(NULL);
        exit_shell_child_status(st);
    }

    if (state->monitor_mode) {
        (void)setpgid(pid, pid);
    }

    state->last_async_pid = pid;
    jobs_track_async(pid, pid, source);
    trace_log(POSISH_TRACE_SIGNALS, "async list pid=%ld", (long)pid);
    return 0;
}

static int run_brace_group_command(struct shell_state *state, const char *body,
                                   const char *redir_suffix) {
    struct redir_vec redirs;
    struct fd_backup_vec backups;
    int status;

    if (parse_redirections_from_source(redir_suffix, state, &redirs) != 0) {
        return 2;
    }

    backups.items = NULL;
    backups.len = 0;
    if (apply_redirections(&redirs, true, state->noclobber, &backups) != 0) {
        fd_backup_restore(&backups);
        redir_vec_free(&redirs);
        return 1;
    }

    status = execute_program_text(state, body);
    fd_backup_restore(&backups);
    redir_vec_free(&redirs);
    return status;
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
    if ((*suffix_out)[0] == '\0') {
        arena_maybe_free(*core_out);
        arena_maybe_free(*suffix_out);
        *core_out = NULL;
        *suffix_out = NULL;
        return false;
    }

    return true;
}

static int execute_command_atom(struct shell_state *state, const char *source,
                                bool allow_builtin) {
    char *collapsed;
    char *trimmed;
    char *inner;
    char *subshell_redirs;
    char *brace_inner;
    char *brace_redirs;
    char *fn_name;
    char *fn_body;
    char *if_cond;
    char *if_then;
    char *if_else;
    char *if_redirs;
    char *while_cond;
    char *while_body;
    char *while_redirs;
    char *for_name;
    char *for_words;
    char *for_body;
    char *for_redir_suffix;
    char *case_core;
    char *case_redir_suffix;
    bool while_is_until;
    bool for_implicit_words;
    int status;

    trimmed = dup_slice(source, 0, strlen(source));
    collapsed = collapse_line_continuations(trimmed);
    arena_maybe_free(trimmed);
    trimmed = dup_trimmed_slice(collapsed, 0, strlen(collapsed));
    arena_maybe_free(collapsed);
    if (trimmed[0] == '\0') {
        arena_maybe_free(trimmed);
        return 0;
    }

    /*
     * `set +n` (or `set +o noexec`) is allowed to run while in noexec mode so
     * scripts can turn execution back on. Everything else is parse-only.
     */
    if (state->noexec && !noexec_allows_set_toggle(trimmed)) {
        arena_maybe_free(trimmed);
        return 0;
    }

    fn_name = NULL;
    fn_body = NULL;
    if_cond = NULL;
    if_then = NULL;
    if_else = NULL;
    if_redirs = NULL;
    while_cond = NULL;
    while_body = NULL;
    while_redirs = NULL;
    for_name = NULL;
    for_words = NULL;
    for_body = NULL;
    for_redir_suffix = NULL;
    case_core = NULL;
    case_redir_suffix = NULL;
    while_is_until = false;
    for_implicit_words = false;
    inner = NULL;
    subshell_redirs = NULL;
    brace_inner = NULL;
    brace_redirs = NULL;

    if (parse_function_definition(trimmed, &fn_name, &fn_body)) {
        status = shell_set_function(state, fn_name, fn_body);
        arena_maybe_free(fn_name);
        arena_maybe_free(fn_body);
        arena_maybe_free(trimmed);
        return status;
    }

    if (ignore_helper_function_declaration(trimmed)) {
        arena_maybe_free(trimmed);
        return 0;
    }

    if (parse_simple_if(trimmed, &if_cond, &if_then, &if_else, &if_redirs)) {
        struct redir_vec if_redir_vec;
        struct fd_backup_vec if_backups;
        bool if_redir_applied;
        bool saved_errexit;

        if_redir_vec.items = NULL;
        if_redir_vec.len = 0;
        if_backups.items = NULL;
        if_backups.len = 0;
        if_redir_applied = false;

        if (if_redirs != NULL && if_redirs[0] != '\0') {
            if (parse_redirections_from_source(if_redirs, state, &if_redir_vec) !=
                0) {
                status = 2;
                goto if_done;
            }
            if (apply_redirections(&if_redir_vec, true, state->noclobber,
                                   &if_backups) != 0) {
                fd_backup_restore(&if_backups);
                status = 1;
                goto if_done;
            }
            if_redir_applied = true;
        }

        /* POSIX: -e does not trigger on commands used as if-conditions. */
        saved_errexit = state->errexit;
        state->errexit = false;
        status = execute_program_text(state, if_cond);
        state->errexit = saved_errexit;
        if (!state->should_exit && !state->return_requested &&
            !has_pending_flow_control(state)) {
            if (status == 0) {
                status = execute_program_text(state, if_then);
            } else if (if_else != NULL) {
                status = execute_program_text(state, if_else);
            } else {
                status = 0;
            }
        }
if_done:
        if (if_redir_applied) {
            fd_backup_restore(&if_backups);
        }
        redir_vec_free(&if_redir_vec);
        arena_maybe_free(if_redirs);
        arena_maybe_free(if_cond);
        arena_maybe_free(if_then);
        arena_maybe_free(if_else);
        arena_maybe_free(trimmed);
        return status;
    }

    if (parse_simple_while(trimmed, &while_cond, &while_body, &while_is_until,
                           &while_redirs)) {
        struct redir_vec while_redir_vec;
        struct fd_backup_vec while_backups;
        bool while_redir_applied;

        status = 0;
        while_redir_vec.items = NULL;
        while_redir_vec.len = 0;
        while_backups.items = NULL;
        while_backups.len = 0;
        while_redir_applied = false;

        if (while_redirs != NULL && while_redirs[0] != '\0') {
            if (parse_redirections_from_source(while_redirs, state,
                                               &while_redir_vec) != 0) {
                status = 2;
                goto while_done;
            }
            if (apply_redirections(&while_redir_vec, true, state->noclobber,
                                   &while_backups) != 0) {
                fd_backup_restore(&while_backups);
                status = 1;
                goto while_done;
            }
            while_redir_applied = true;
        }

        state->loop_depth++;
        while (!state->should_exit) {
            int cond_status;
            bool saved_errexit;

            /* POSIX: -e does not trigger on while/until condition commands. */
            saved_errexit = state->errexit;
            state->errexit = false;
            cond_status = execute_program_text(state, while_cond);
            state->errexit = saved_errexit;
            if (state->should_exit || state->return_requested) {
                break;
            }
            if ((!while_is_until && cond_status != 0) ||
                (while_is_until && cond_status == 0)) {
                break;
            }
            status = execute_program_text(state, while_body);
            if (state->should_exit || state->return_requested) {
                break;
            }
            if (state->break_levels > 0) {
                state->break_levels--;
                status = 0;
                break;
            }
            if (state->continue_levels > 0) {
                state->continue_levels--;
                status = 0;
                if (state->continue_levels > 0) {
                    break;
                }
                continue;
            }
        }
        state->loop_depth--;
        if (while_redir_applied) {
            fd_backup_restore(&while_backups);
        }
        redir_vec_free(&while_redir_vec);
while_done:
        arena_maybe_free(while_redirs);
        arena_maybe_free(while_cond);
        arena_maybe_free(while_body);
        arena_maybe_free(trimmed);
        return status;
    }

    if (parse_simple_for(trimmed, &for_name, &for_words, &for_body,
                         &for_implicit_words, &for_redir_suffix)) {
        struct token_vec for_lexed;
        struct word_vec for_raw_words;
        struct redir_vec for_redirs;
        struct token_vec for_expanded;
        struct token_vec for_in;
        struct redir_vec for_loop_redirs;
        struct fd_backup_vec for_backups;
        bool for_redir_applied;
        size_t i;

        for_lexed.items = NULL;
        for_lexed.len = 0;
        for_raw_words.items = NULL;
        for_raw_words.len = 0;
        for_redirs.items = NULL;
        for_redirs.len = 0;
        for_expanded.items = NULL;
        for_expanded.len = 0;
        for_loop_redirs.items = NULL;
        for_loop_redirs.len = 0;
        for_backups.items = NULL;
        for_backups.len = 0;
        for_redir_applied = false;
        status = 0;

        if (for_redir_suffix != NULL && for_redir_suffix[0] != '\0') {
            if (parse_redirections_from_source(for_redir_suffix, state,
                                               &for_loop_redirs) != 0) {
                status = 2;
                goto for_done;
            }
            if (apply_redirections(&for_loop_redirs, true, state->noclobber,
                                   &for_backups) != 0) {
                fd_backup_restore(&for_backups);
                status = 1;
                goto for_done;
            }
            for_redir_applied = true;
        }

        if (for_implicit_words) {
            for_expanded.items = arena_xmalloc(sizeof(*for_expanded.items) *
                                               state->positional_count);
            for_expanded.len = state->positional_count;
            for (i = 0; i < state->positional_count; i++) {
                for_expanded.items[i] = arena_xstrdup(state->positional_params[i]);
            }
        } else if (for_words[0] != '\0') {
            if (lexer_split_words(for_words, &for_lexed) != 0) {
                status = 2;
                goto for_done;
            }
            if (collect_words_and_redirs(&for_lexed, &for_raw_words, &for_redirs) != 0) {
                status = 2;
                goto for_done;
            }
            if (for_redirs.len != 0) {
                posish_errorf("for: redirection in word list is not supported");
                status = 2;
                goto for_done;
            }

            for_in.items = for_raw_words.items;
            for_in.len = for_raw_words.len;
            if (expand_words(&for_in, &for_expanded, state, true) != 0) {
                status = 2;
                goto for_done;
            }
        }

        state->loop_depth++;
        for (i = 0; i < for_expanded.len && !state->should_exit; i++) {
            if (vars_set_assignment(state, for_name, for_expanded.items[i], true) !=
                0) {
                status = 1;
                if (!state->interactive) {
                    state->should_exit = true;
                    state->exit_status = status;
                }
                break;
            }

            status = execute_program_text(state, for_body);
            if (state->should_exit || state->return_requested) {
                break;
            }
            if (state->break_levels > 0) {
                state->break_levels--;
                status = 0;
                break;
            }
            if (state->continue_levels > 0) {
                state->continue_levels--;
                status = 0;
                if (state->continue_levels > 0) {
                    break;
                }
            }
        }
        state->loop_depth--;
        if (for_redir_applied) {
            fd_backup_restore(&for_backups);
        }

for_done:
        lexer_free_tokens(&for_lexed);
        word_vec_free(&for_raw_words);
        redir_vec_free(&for_redirs);
        lexer_free_tokens(&for_expanded);
        redir_vec_free(&for_loop_redirs);
        arena_maybe_free(for_redir_suffix);
        arena_maybe_free(for_name);
        arena_maybe_free(for_words);
        arena_maybe_free(for_body);
        arena_maybe_free(trimmed);
        return status;
    }

    if (split_case_redirection_suffix(trimmed, &case_core, &case_redir_suffix)) {
        struct redir_vec case_redirs;
        struct fd_backup_vec case_backups;

        case_redirs.items = NULL;
        case_redirs.len = 0;
        case_backups.items = NULL;
        case_backups.len = 0;
        status = 2;

        if (parse_redirections_from_source(case_redir_suffix, state,
                                           &case_redirs) != 0) {
            goto case_done;
        }
        if (apply_redirections(&case_redirs, true, state->noclobber,
                               &case_backups) != 0) {
            fd_backup_restore(&case_backups);
            status = 1;
            goto case_done;
        }

        if (try_execute_case_command(state, case_core, &status,
                                     execute_program_text)) {
            fd_backup_restore(&case_backups);
        } else {
            fd_backup_restore(&case_backups);
            status = 2;
        }

case_done:
        redir_vec_free(&case_redirs);
        arena_maybe_free(case_core);
        arena_maybe_free(case_redir_suffix);
        arena_maybe_free(trimmed);
        return status;
    }

    if (try_execute_case_command(state, trimmed, &status, execute_program_text)) {
        arena_maybe_free(trimmed);
        return status;
    }

    if (try_execute_alt_parameter_command(state, trimmed, &status)) {
        arena_maybe_free(trimmed);
        return status;
    }

    if (unwrap_subshell_group(trimmed, &inner, &subshell_redirs)) {
        status = run_subshell_group_command(state, inner, subshell_redirs);
        arena_maybe_free(inner);
        arena_maybe_free(subshell_redirs);
    } else if (unwrap_brace_group(trimmed, &brace_inner, &brace_redirs)) {
        status = run_brace_group_command(state, brace_inner, brace_redirs);
        arena_maybe_free(brace_inner);
        arena_maybe_free(brace_redirs);
    } else {
        status = execute_simple_command(state, trimmed, allow_builtin);
    }

    arena_maybe_free(trimmed);
    return status;
}

static void exec_child_command(struct shell_state *parent_state, const char *source) {
    struct shell_state local_state;
    struct arena_mark child_mark;
    int status;

    local_state = *parent_state;
    arena_init(&local_state.arena_perm, parent_state->arena_perm.default_block_size);
    arena_init(&local_state.arena_script,
               parent_state->arena_script.default_block_size);
    arena_init(&local_state.arena_cmd, parent_state->arena_cmd.default_block_size);
    arena_set_current(&local_state.arena_cmd);
    arena_mark_take(&local_state.arena_cmd, &child_mark);
    local_state.should_exit = false;
    local_state.exit_status = 0;
    local_state.running_signal_trap = false;
    local_state.running_exit_trap = false;
    local_state.main_context = false;

    status = execute_command_atom(&local_state, source, true);
    arena_mark_rewind(&local_state.arena_cmd, &child_mark);
    if (local_state.should_exit) {
        status = local_state.exit_status;
    }
    fflush(NULL);
    _exit(status);
}

static int execute_pipeline(struct shell_state *state, const char *source) {
    char *normalized;
    char *work;
    char *cursor;
    bool negate;
    size_t i;
    size_t start;
    char quote;
    int paren_depth;
    int brace_depth;
    int if_depth;
    int case_depth;
    int loop_depth;
    char **commands;
    size_t cmd_len;
    pid_t *pids;
    pid_t pipeline_pgid;
    bool isolate_pipeline_pgid;
    bool pipefail_snapshot;
    int *command_statuses;
    int *wait_statuses;
    bool *have_wait_statuses;
    int last_status;
    int in_fd;

    normalized = collapse_line_continuations(source);
    work = dup_trimmed_slice(normalized, 0, strlen(normalized));
    arena_maybe_free(normalized);
    cursor = work;
    negate = false;

    for (;;) {
        while (isspace((unsigned char)*cursor)) {
            cursor++;
        }

        if (*cursor != '!') {
            break;
        }
        if (cursor[1] != '\0' && !isspace((unsigned char)cursor[1]) && cursor[1] != '(') {
            break;
        }

        negate = !negate;
        cursor++;
    }

    commands = NULL;
    cmd_len = 0;
    quote = '\0';
    paren_depth = 0;
    brace_depth = 0;
    if_depth = 0;
    case_depth = 0;
    loop_depth = 0;
    start = 0;

    for (i = 0;; i++) {
        char ch;
        bool delim;

        ch = cursor[i];
        delim = false;

        if (ch == '\0') {
            delim = true;
        } else if (quote == '\0') {
            if (ch == '\\' && cursor[i + 1] != '\0') {
                i++;
                continue;
            }
            if (paren_depth == 0 && brace_depth == 0 &&
                (isalpha((unsigned char)ch) || ch == '_') &&
                word_starts_command_position(cursor, i)) {
                size_t j;
                size_t boundary;
                char keyword[16];
                size_t kwlen;

                j = i;
                kwlen = 0;
                while (cursor[j] != '\0') {
                    if (cursor[j] == '\\' && cursor[j + 1] == '\n') {
                        j += 2;
                        continue;
                    }
                    if (!isalnum((unsigned char)cursor[j]) && cursor[j] != '_') {
                        break;
                    }
                    if (kwlen + 1 < sizeof(keyword)) {
                        keyword[kwlen] = cursor[j];
                    }
                    kwlen++;
                    j++;
                }
                boundary = skip_continuations_forward(cursor, j);
                if (keyword_boundary(cursor[boundary]) &&
                    cursor[boundary] != ')') {
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
                                   keyword_preceded_by_list_separator(cursor, i)) {
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
                       ch == '|' &&
                       cursor[i + 1] != '|' &&
                       !(i > 0 && cursor[i - 1] == '|') &&
                       !(i > 0 && cursor[i - 1] == '>')) {
                delim = true;
            }
        } else if (quote == '\'' && ch == '\'') {
            quote = '\0';
        } else if (quote == '"') {
            if (ch == '\\' && cursor[i + 1] != '\0') {
                i++;
                continue;
            }
            if (ch == '"') {
                quote = '\0';
            }
        }

        if (delim) {
            char *part;

            part = dup_trimmed_slice(cursor, start, i);
            if (part[0] != '\0') {
                commands = xrealloc(commands, sizeof(*commands) * (cmd_len + 1));
                commands[cmd_len++] = part;
            } else {
                arena_maybe_free(part);
            }

            if (ch == '\0') {
                break;
            }

            start = i + 1;
        }
    }

    if (cmd_len == 0) {
        arena_maybe_free(commands);
        arena_maybe_free(work);
        return 0;
    }

    if (cmd_len == 1) {
        int status;
        bool ignored;

        status = execute_command_atom(state, commands[0], true);
        /*
         * Preserve set -e suppression from nested execution contexts
         * (for example the left side of && inside grouping/if bodies).
         */
        ignored = state->errexit_ignored;
        state->errexit_ignored = status != 0 && ignored;
        state->last_status = status;
        free_string_vec(commands, cmd_len);
        arena_maybe_free(work);
        if (negate) {
            state->errexit_ignored = true;
            state->last_status = status == 0 ? 1 : 0;
            return state->last_status;
        }
        return status;
    }

    pids = arena_xmalloc(sizeof(*pids) * cmd_len);
    command_statuses = arena_xmalloc(sizeof(*command_statuses) * cmd_len);
    wait_statuses = arena_xmalloc(sizeof(*wait_statuses) * cmd_len);
    have_wait_statuses = arena_xmalloc(sizeof(*have_wait_statuses) * cmd_len);
    memset(wait_statuses, 0, sizeof(*wait_statuses) * cmd_len);
    memset(have_wait_statuses, 0, sizeof(*have_wait_statuses) * cmd_len);
    pipeline_pgid = -1;
    isolate_pipeline_pgid = state->monitor_mode && state->main_context;
    pipefail_snapshot = state->pipefail;
    in_fd = -1;

    for (i = 0; i < cmd_len; i++) {
        int pipefd[2];
        pid_t pid;

        pipefd[0] = -1;
        pipefd[1] = -1;

        if (i + 1 < cmd_len) {
            if (pipe(pipefd) != 0) {
                perror("pipe");
                free_string_vec(commands, cmd_len);
                arena_maybe_free(pids);
                arena_maybe_free(command_statuses);
                arena_maybe_free(wait_statuses);
                arena_maybe_free(have_wait_statuses);
                arena_maybe_free(work);
                return 1;
            }
        }

        pid = fork();
        if (pid < 0) {
            perror("fork");
            if (pipefd[0] >= 0) {
                close(pipefd[0]);
                close(pipefd[1]);
            }
            free_string_vec(commands, cmd_len);
            arena_maybe_free(pids);
            arena_maybe_free(command_statuses);
            arena_maybe_free(wait_statuses);
            arena_maybe_free(have_wait_statuses);
            arena_maybe_free(work);
            return 1;
        }

        if (pid == 0) {
            if (isolate_pipeline_pgid) {
                pid_t target_pgid;

                /*
                 * Top-level monitor mode keeps pipeline children in their own
                 * process group so group-targeted signals do not hit the shell.
                 */
                target_pgid = (i == 0) ? 0 : pipeline_pgid;
                if (setpgid(0, target_pgid) != 0 && errno != EACCES &&
                    errno != ESRCH && errno != EPERM && errno != EINVAL) {
                    _exit(1);
                }
            }

            if (in_fd >= 0) {
                if (dup2(in_fd, STDIN_FILENO) < 0) {
                    perror("dup2");
                    _exit(1);
                }
            }
            if (pipefd[1] >= 0) {
                if (dup2(pipefd[1], STDOUT_FILENO) < 0) {
                    perror("dup2");
                    _exit(1);
                }
            }

            if (in_fd >= 0) {
                close(in_fd);
            }
            if (pipefd[0] >= 0) {
                close(pipefd[0]);
            }
            if (pipefd[1] >= 0) {
                close(pipefd[1]);
            }

            exec_child_command(state, commands[i]);
        }

        pids[i] = pid;
        if (isolate_pipeline_pgid) {
            if (pipeline_pgid <= 0) {
                pipeline_pgid = pid;
            }
            if (setpgid(pid, pipeline_pgid) != 0 && errno != EACCES &&
                errno != ESRCH && errno != EPERM && errno != EINVAL) {
                /* keep running: parent/child setpgid races are non-fatal */
            }
        }

        if (in_fd >= 0) {
            close(in_fd);
        }
        if (pipefd[1] >= 0) {
            close(pipefd[1]);
        }
        in_fd = pipefd[0];
    }

    if (in_fd >= 0) {
        close(in_fd);
    }

    last_status = 0;
    for (i = 0; i < cmd_len; i++) {
        int wstatus;
        pid_t w;
        int command_status;

        for (;;) {
            w = waitpid(pids[i], &wstatus, WUNTRACED);
            if (w < 0 && errno == EINTR) {
                shell_run_pending_traps(state);
                continue;
            }
            break;
        }

        if (w < 0) {
            perror("waitpid");
            command_statuses[i] = 1;
            continue;
        }

        wait_statuses[i] = wstatus;
        have_wait_statuses[i] = true;

        if (WIFEXITED(wstatus)) {
            command_status = WEXITSTATUS(wstatus);
        } else if (WIFSTOPPED(wstatus)) {
            pid_t job_pgid;
            pid_t status_pid;
            size_t j;

            job_pgid = isolate_pipeline_pgid && pipeline_pgid > 0 ? pipeline_pgid
                                                                  : pids[i];
            status_pid = pids[cmd_len - 1];
            jobs_track_job(job_pgid, pids, cmd_len, status_pid, source, true);
            for (j = 0; j < cmd_len; j++) {
                if (have_wait_statuses[j]) {
                    jobs_note_process_status(pids[j], wait_statuses[j]);
                }
            }
            command_status = shell_status_from_wait_status(wstatus);
        } else if (WIFSIGNALED(wstatus)) {
            command_status = shell_status_from_wait_status(wstatus);
        } else {
            command_status = 1;
        }
        if (!WIFSTOPPED(wstatus)) {
            struct jobs_entry_info tracked_job;

            if (jobs_find_by_pid(pids[i], &tracked_job)) {
                jobs_note_process_status(pids[i], wstatus);
            }
        }
        command_statuses[i] = command_status;
    }

    if (pipefail_snapshot) {
        int last_non_zero;

        last_non_zero = 0;
        for (i = 0; i < cmd_len; i++) {
            if (command_statuses[i] != 0) {
                last_non_zero = command_statuses[i];
            }
        }
        last_status = last_non_zero;
    } else {
        last_status = command_statuses[cmd_len - 1];
    }

    free_string_vec(commands, cmd_len);
    arena_maybe_free(pids);
    arena_maybe_free(command_statuses);
    arena_maybe_free(wait_statuses);
    arena_maybe_free(have_wait_statuses);
    arena_maybe_free(work);

    if (negate) {
        state->errexit_ignored = true;
        state->last_status = last_status == 0 ? 1 : 0;
        return state->last_status;
    }
    state->errexit_ignored = false;
    state->last_status = last_status;
    return last_status;
}

static int execute_andor(struct shell_state *state, const char *source) {
    char *normalized;
    size_t i;
    size_t start;
    char quote;
    int paren_depth;
    int brace_depth;
    int if_depth;
    int case_depth;
    int loop_depth;
    char **parts;
    enum andor_op *ops;
    size_t part_len;
    size_t op_len;
    int status;
    bool errexit_ignored;

    normalized = collapse_line_continuations(source);
    source = normalized;

    /*
     * Compound loop commands can contain &&/|| internally in their bodies.
     * Parse them as a single pipeline atom here to avoid premature splitting.
     */
    if (compound_needs_single_atom(source)) {
        status = execute_pipeline(state, source);
        arena_maybe_free(normalized);
        return status;
    }

    parts = NULL;
    ops = NULL;
    part_len = 0;
    op_len = 0;
    quote = '\0';
    paren_depth = 0;
    brace_depth = 0;
    if_depth = 0;
    case_depth = 0;
    loop_depth = 0;
    start = 0;
    errexit_ignored = false;

    for (i = 0;; i++) {
        char ch;
        bool delim;

        ch = source[i];
        delim = false;

        if (ch == '\0') {
            delim = true;
        } else if (quote == '\0') {
            if (ch == '\\' && source[i + 1] != '\0') {
                i++;
                continue;
            }
            if (ch == '$' && source[i + 1] == '(') {
                size_t end;

                if (find_command_subst_end(source, i, &end)) {
                    i = end;
                    continue;
                }
            }
            if (ch == '$' && source[i + 1] == '\'') {
                size_t end;

                if (find_dollar_single_quote_end(source, i, &end)) {
                    i = end;
                    continue;
                }
            }
            if (ch == '\'' || ch == '"') {
                quote = ch;
            } else if (paren_depth == 0 && brace_depth == 0 &&
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
                if (keyword_boundary(source[boundary]) &&
                    source[boundary] != ')') {
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
                                   keyword_preceded_by_list_separator(source, i)) {
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
                       ch == '&' &&
                       source[i + 1] == '&') {
                delim = true;
            } else if (paren_depth == 0 && brace_depth == 0 &&
                       if_depth == 0 && case_depth == 0 && loop_depth == 0 &&
                       ch == '|' &&
                       source[i + 1] == '|') {
                delim = true;
            }
        } else if (quote == '\'' && ch == '\'') {
            quote = '\0';
        } else if (quote == '"') {
            if (ch == '$' && source[i + 1] == '(') {
                size_t end;

                if (find_command_subst_end(source, i, &end)) {
                    i = end;
                    continue;
                }
            }
            if (ch == '\\' && source[i + 1] != '\0') {
                i++;
                continue;
            }
            if (ch == '"') {
                quote = '\0';
            }
        }

        if (delim) {
            char *part;

            part = dup_trimmed_slice(source, start, i);
            if (part[0] != '\0') {
                parts = xrealloc(parts, sizeof(*parts) * (part_len + 1));
                parts[part_len++] = part;
            } else {
                arena_maybe_free(part);
            }

            if (ch == '\0') {
                break;
            }

            if (source[i] == '&' && source[i + 1] == '&') {
                ops = xrealloc(ops, sizeof(*ops) * (op_len + 1));
                ops[op_len++] = ANDOR_AND;
            } else if (source[i] == '|' && source[i + 1] == '|') {
                ops = xrealloc(ops, sizeof(*ops) * (op_len + 1));
                ops[op_len++] = ANDOR_OR;
            }

            i++;
            start = i + 1;
        }
    }

    if (part_len == 0) {
        arena_maybe_free(parts);
        arena_maybe_free(ops);
        arena_maybe_free(normalized);
        return 0;
    }

    status = execute_pipeline(state, parts[0]);
    errexit_ignored = state->errexit_ignored;
    if (state->should_exit || has_pending_flow_control(state)) {
        free_string_vec(parts, part_len);
        arena_maybe_free(ops);
        arena_maybe_free(normalized);
        state->errexit_ignored = status != 0 && errexit_ignored;
        return status;
    }

    for (i = 0; i < op_len && i + 1 < part_len; i++) {
        if (ops[i] == ANDOR_AND) {
            if (status == 0) {
                status = execute_pipeline(state, parts[i + 1]);
                errexit_ignored = state->errexit_ignored;
            } else {
                /*
                 * set -e is suppressed for a non-final command that failed on
                 * the left side of && and short-circuited the list.
                 */
                errexit_ignored = true;
            }
        } else {
            if (status != 0) {
                status = execute_pipeline(state, parts[i + 1]);
                errexit_ignored = state->errexit_ignored;
            }
        }
        if (state->should_exit) {
            break;
        }
        if (has_pending_flow_control(state)) {
            break;
        }
    }

    free_string_vec(parts, part_len);
    arena_maybe_free(ops);
    arena_maybe_free(normalized);
    state->errexit_ignored = status != 0 && errexit_ignored;
    return status;
}

static bool command_text_needs_more_input(const char *source,
                                          bool include_heredoc) {
    size_t len;

    if (source == NULL) {
        return false;
    }

    len = strlen(source);
    return shell_needs_more_input_text_mode(source, len, include_heredoc) != 0;
}

char *exec_alias_expand_preview(struct shell_state *state, const char *source) {
    char *logical;
    char *part;
    char *rewritten;
    bool changed;

    if (source == NULL) {
        return NULL;
    }

    logical = collapse_line_continuations(source);
    part = dup_trimmed_slice(logical, 0, strlen(logical));
    arena_maybe_free(logical);
    if (part[0] == '\0') {
        arena_maybe_free(part);
        return NULL;
    }

    rewritten = NULL;
    changed = false;
    if (alias_rewrite_snippet(state, part, &rewritten, &changed) != 0) {
        arena_maybe_free(part);
        return NULL;
    }
    if (changed) {
        arena_maybe_free(part);
        part = rewritten;
    } else {
        arena_maybe_free(rewritten);
        arena_maybe_free(part);
        return NULL;
    }

    if (part[0] == '\0') {
        arena_maybe_free(part);
        return NULL;
    }
    return part;
}

bool exec_alias_preview_needs_more(const char *preview) {
    if (preview == NULL || preview[0] == '\0') {
        return false;
    }
    return command_text_needs_more_input(preview, false);
}

static int execute_program_text(struct shell_state *state, const char *source) {
    return execute_program_text_internal(state, source, true);
}

static int execute_program_text_internal(struct shell_state *state,
                                         const char *source,
                                         bool apply_aliases) {
    size_t i;
    size_t start;
    char quote;
    int paren_depth;
    int brace_depth;
    int if_depth;
    int case_depth;
    int loop_depth;
    int status;
    bool skip_next_done;
    bool pending_heredoc;
    char *pending_function_head;
    char *pending_raw;
    size_t pending_start;
    struct arena *saved_arena;
    struct arena_mark program_mark;
    bool have_program_mark;

    quote = '\0';
    paren_depth = 0;
    brace_depth = 0;
    if_depth = 0;
    case_depth = 0;
    loop_depth = 0;
    start = 0;
    status = 0;
    skip_next_done = false;
    pending_heredoc = false;
    pending_function_head = NULL;
    pending_raw = NULL;
    pending_start = 0;
    saved_arena = arena_get_current();
    have_program_mark = saved_arena != NULL;
    if (have_program_mark) {
        arena_mark_take(saved_arena, &program_mark);
    }

    for (i = 0;; i++) {
        char ch;
        bool delim;

        ch = source[i];
        delim = false;

        if (ch == '\0') {
            delim = true;
        } else if (quote == '\0') {
            if (ch == '\\' && source[i + 1] != '\0') {
                i++;
                continue;
            }
            if (ch == '$' && source[i + 1] == '(') {
                size_t end;

                if (find_command_subst_end(source, i, &end)) {
                    i = end;
                    continue;
                }
            }
            if (ch == '$' && source[i + 1] == '\'') {
                size_t end;

                if (find_dollar_single_quote_end(source, i, &end)) {
                    i = end;
                    continue;
                }
            }
            if (ch == '\'' || ch == '"') {
                quote = ch;
            } else if (paren_depth == 0 && brace_depth == 0 &&
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
                if (keyword_boundary(source[boundary]) &&
                    source[boundary] != ')') {
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
                                   keyword_preceded_by_list_separator(source, i)) {
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
            } else if (paren_depth == 0 && brace_depth == 0 && ch == '<' &&
                       source[i + 1] == '<') {
                pending_heredoc = true;
            } else if (paren_depth == 0 && brace_depth == 0 &&
                       if_depth == 0 && case_depth == 0 && loop_depth == 0 &&
                       ((ch == ';' && !pending_heredoc) ||
                        (ch == '\n' &&
                         (!newline_continues_command(source, i) ||
                          pending_heredoc)) ||
                        /*
                         * Treat only a control-operator '&' as async
                         * separator. Exclude '&&' and redirection forms
                         * like '<&' / '>&'.
                         */
                        (is_async_separator_amp(source, i) &&
                         !pending_heredoc))) {
                delim = true;
            }
        } else if (quote == '\'' && ch == '\'') {
            quote = '\0';
        } else if (quote == '"') {
            if (ch == '$' && source[i + 1] == '(') {
                size_t end;

                if (find_command_subst_end(source, i, &end)) {
                    i = end;
                    continue;
                }
            }
            if (ch == '\\' && source[i + 1] != '\0') {
                i++;
                continue;
            }
            if (ch == '"') {
                quote = '\0';
            }
        }

        if (delim) {
            char *chunk_raw;
            char *raw_part;
            char *part;
            char *logical_part;
            size_t command_start;
            bool heredoc_handled;
            size_t heredoc_new_pos;
            int heredoc_rc;
            heredoc_command_runner_fn heredoc_runner;

            chunk_raw = dup_slice(source, start, i);
            if (pending_raw != NULL) {
                size_t pending_len;
                size_t chunk_len;

                pending_len = strlen(pending_raw);
                chunk_len = strlen(chunk_raw);
                raw_part = arena_xmalloc(pending_len + chunk_len + 1);
                memcpy(raw_part, pending_raw, pending_len);
                memcpy(raw_part + pending_len, chunk_raw, chunk_len + 1);
                command_start = pending_start;
                arena_maybe_free(pending_raw);
                pending_raw = NULL;
                arena_maybe_free(chunk_raw);
            } else {
                raw_part = chunk_raw;
                command_start = start;
            }

            logical_part = collapse_line_continuations(raw_part);
            part = dup_trimmed_slice(logical_part, 0, strlen(logical_part));
            arena_maybe_free(logical_part);
            if (part[0] != '\0') {
                bool alias_changed;

                alias_changed = false;
                if (apply_aliases) {
                    char *alias_rewritten_part;

                    alias_rewritten_part = NULL;
                    if (alias_rewrite_snippet(state, part,
                                                    &alias_rewritten_part,
                                                    &alias_changed) != 0) {
                        arena_maybe_free(part);
                        arena_maybe_free(raw_part);
                        status = 2;
                        break;
                    }
                    if (alias_changed) {
                        arena_maybe_free(part);
                        part = alias_rewritten_part;
                    } else {
                        arena_maybe_free(alias_rewritten_part);
                    }

                    if (part[0] == '\0') {
                        /* Alias-expanded blank commands preserve prior $? state. */
                        status = state->last_status;
                        arena_maybe_free(part);
                        arena_maybe_free(raw_part);
                        if (ch == '\0') {
                            break;
                        }
                        start = i + 1;
                        pending_heredoc = false;
                        continue;
                    }

                    if (alias_changed && ch != '\0' &&
                        command_text_needs_more_input(part, false)) {
                        size_t raw_len;

                        raw_len = strlen(raw_part);
                        pending_raw = arena_xmalloc(raw_len + 2);
                        memcpy(pending_raw, raw_part, raw_len);
                        pending_raw[raw_len] = ch;
                        pending_raw[raw_len + 1] = '\0';
                        pending_start = command_start;
                        arena_maybe_free(part);
                        arena_maybe_free(raw_part);
                        start = i + 1;
                        pending_heredoc = false;
                        continue;
                    }
                }

                if (pending_function_head != NULL) {
                    size_t hlen;
                    size_t plen;
                    char *combined;

                    hlen = strlen(pending_function_head);
                    plen = strlen(part);
                    combined = arena_xmalloc(hlen + 1 + plen + 1);
                    memcpy(combined, pending_function_head, hlen);
                    combined[hlen] = '\n';
                    memcpy(combined + hlen + 1, part, plen + 1);
                    arena_maybe_free(pending_function_head);
                    arena_maybe_free(part);
                    pending_function_head = NULL;
                    part = combined;
                }

                if (looks_like_function_header_only(part) && ch != '\0') {
                    pending_function_head = part;
                    arena_maybe_free(raw_part);
                    start = i + 1;
                    pending_heredoc = false;
                    continue;
                }

                /* Keep $LINENO aligned to each top-level command start. */
                set_lineno_for_command(source, command_start);
                state->errexit_ignored = false;
                if (skip_next_done && strcmp(part, "done") == 0) {
                    skip_next_done = false;
                    status = 0;
                } else if (ignore_helper_function_declaration(part)) {
                    skip_next_done = true;
                    status = 0;
                } else {
                    char *fn_probe_name;
                    char *fn_probe_body;
                    bool snippet_is_function_def;
                    bool preserve_heredoc_tempfiles;

                    fn_probe_name = NULL;
                    fn_probe_body = NULL;
                    snippet_is_function_def =
                        parse_function_definition(part, &fn_probe_name, &fn_probe_body);
                    arena_maybe_free(fn_probe_name);
                    arena_maybe_free(fn_probe_body);

                    /*
                     * Function definitions with here-doc redirections must keep
                     * their captured input available for later function calls.
                     */
                    preserve_heredoc_tempfiles = snippet_is_function_def;
                    heredoc_runner = command_requires_program_runner(part)
                                         ? execute_program_text
                                         : execute_andor;
                    if (snippet_is_function_def) {
                        heredoc_runner = execute_program_text;
                    }
                    heredoc_rc = maybe_execute_heredoc_command(
                        state, part, source, i + 1, &heredoc_new_pos,
                        &heredoc_handled, &status, heredoc_runner,
                        preserve_heredoc_tempfiles);
                    if (heredoc_rc != 0) {
                        status = 1;
                        state->last_status = status;
                        if (status != 0 && state->errexit && !state->interactive &&
                            !state->errexit_ignored) {
                            state->should_exit = true;
                            state->exit_status = status;
                        }
                    } else if (heredoc_handled) {
                        state->last_status = status;
                        if (status != 0 && state->errexit && !state->interactive &&
                            !state->errexit_ignored) {
                            state->should_exit = true;
                            state->exit_status = status;
                        }
                        arena_maybe_free(part);
                        arena_maybe_free(raw_part);
                        if (state->should_exit) {
                            break;
                        }
                        start = heredoc_new_pos;
                        pending_heredoc = false;
                        i = start == 0 ? 0 : start - 1;
                        continue;
                    }

                    if (ch == '&') {
                        status = run_async_list(state, part);
                        state->last_status = status;
                    } else {
                        if (apply_aliases && alias_changed &&
                            command_requires_program_runner(part)) {
                            char *alias_cleaned;

                            /*
                             * Alias-expanded snippets can inject comments via
                             * embedded newlines; normalize them like top-level
                             * program execution before recursive splitting.
                             */
                            alias_cleaned = strip_comments(part);
                            status = execute_program_text_internal(state,
                                                                   alias_cleaned,
                                                                   false);
                            arena_maybe_free(alias_cleaned);
                        } else {
                            status = execute_andor(state, part);
                        }
                        state->last_status = status;
                        if (status != 0 && state->errexit && !state->interactive &&
                            !state->errexit_ignored) {
                            state->should_exit = true;
                            state->exit_status = status;
                        }
                    }
                }
                shell_run_pending_traps(state);
                if (state->should_exit) {
                    arena_maybe_free(part);
                    arena_maybe_free(raw_part);
                    break;
                }
                if (has_pending_flow_control(state)) {
                    arena_maybe_free(part);
                    arena_maybe_free(raw_part);
                    break;
                }
            }
            arena_maybe_free(part);
            arena_maybe_free(raw_part);

            if (ch == '\0') {
                break;
            }

            start = i + 1;
            pending_heredoc = false;
        }
    }

    if (pending_function_head != NULL) {
        status = execute_andor(state, pending_function_head);
        arena_maybe_free(pending_function_head);
    }
    arena_maybe_free(pending_raw);
    if (have_program_mark) {
        arena_mark_rewind(saved_arena, &program_mark);
        arena_set_current(saved_arena);
    }

    return status;
}

int exec_run_program(struct shell_state *state, const struct ast_program *program) {
    char *normalized;
    char *cleaned;
    int status;

    if (program_contains_quoted_heredoc(program->source)) {
        /*
         * Preserve physical lines for quoted here-doc bodies so backslashes and
         * continuations stay literal inside the body text.
         */
        normalized = arena_xstrdup(program->source);
    } else {
        normalized = collapse_line_continuations(program->source);
    }
    cleaned = strip_comments(normalized);
    arena_maybe_free(normalized);
    status = execute_program_text(state, cleaned);
    arena_maybe_free(cleaned);
    return status;
}
