/* SPDX-License-Identifier: 0BSD */

/* posish - shell runtime */

#define _POSIX_C_SOURCE 200809L

#include "shell.h"

#include "ast.h"
#include "error.h"
#include "expand.h"
#include "exec.h"
#include "lexer.h"
#include "parser.h"
#include "prompt.h"
#include "signals.h"
#include "trace.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define POSISH_ALIAS_ENV_PREFIX "POSISH_ALIAS_"

struct heredoc_marker {
    char *delimiter;
    bool strip_tabs;
};

struct pending_heredoc_state {
    struct heredoc_marker *markers;
    size_t marker_count;
    size_t marker_index;
    bool active;
};

static size_t skip_braced_param(const char *buf, size_t i, size_t len,
                                bool dquote_context);
static bool collect_heredoc_markers(const char *buf, size_t len,
                                    struct heredoc_marker **markers_out,
                                    size_t *count_out);
static int run_startup_path(struct shell_state *state, const char *path,
                            bool interactive);
static int expand_startup_path(struct shell_state *state, const char *text,
                               char **out);
static char *home_startup_path(const char *leaf);

static size_t skip_backtick_subst(const char *buf, size_t i, size_t len) {
    i++;
    while (i < len) {
        if (buf[i] == '\\' && i + 1 < len) {
            i += 2;
            continue;
        }
        if (buf[i] == '`') {
            return i + 1;
        }
        i++;
    }
    return len;
}

static size_t skip_dollar_single_quote(const char *buf, size_t i, size_t len) {
    if (i + 1 >= len || buf[i] != '$' || buf[i + 1] != '\'') {
        return i + 1;
    }

    i += 2;
    while (i < len) {
        if (buf[i] == '\\' && i + 1 < len) {
            i += 2;
            continue;
        }
        if (buf[i] == '\'') {
            return i + 1;
        }
        i++;
    }
    return len + 1;
}

static size_t skip_balanced_parens(const char *buf, size_t i, size_t len) {
    int depth;
    char quote;

    depth = 1;
    quote = '\0';
    while (i < len) {
        char ch;

        ch = buf[i];
        if (quote == '\0') {
            if (ch == '\\' && i + 1 < len) {
                i += 2;
                continue;
            }
            if (ch == '$' && i + 1 < len && buf[i + 1] == '(') {
                i += 2;
                if (i < len && buf[i] == '(') {
                    i++;
                }
                i = skip_balanced_parens(buf, i, len);
                continue;
            }
            if (ch == '$' && i + 1 < len && buf[i + 1] == '{') {
                i += 2;
                i = skip_braced_param(buf, i, len, false);
                continue;
            }
            if (ch == '$' && i + 1 < len && buf[i + 1] == '\'') {
                i = skip_dollar_single_quote(buf, i, len);
                if (i > len) {
                    return len;
                }
                continue;
            }
            if (ch == '`') {
                i = skip_backtick_subst(buf, i, len);
                continue;
            }
            if (ch == '\'' || ch == '"') {
                quote = ch;
                i++;
                continue;
            }
            if (ch == '`') {
                i = skip_backtick_subst(buf, i, len);
                continue;
            }
            if (ch == '(') {
                depth++;
                i++;
                continue;
            }
            if (ch == ')') {
                depth--;
                i++;
                if (depth == 0) {
                    return i;
                }
                continue;
            }
            i++;
            continue;
        }

        if (quote == '\'' && ch == '\'') {
            quote = '\0';
            i++;
            continue;
        }
        if (quote == '"' && ch == '\\' && i + 1 < len) {
            i += 2;
            continue;
        }
        if (quote == '"' && ch == '"') {
            quote = '\0';
        }
        i++;
    }
    return len + 1;
}

static size_t skip_braced_param(const char *buf, size_t i, size_t len,
                                bool dquote_context) {
    int depth;
    char quote;

    depth = 1;
    quote = '\0';
    while (i < len) {
        char ch;

        ch = buf[i];
        if (quote == '\0') {
            if (ch == '\\' && i + 1 < len) {
                i += 2;
                continue;
            }
            if (ch == '$' && i + 1 < len && buf[i + 1] == '(') {
                i += 2;
                if (i < len && buf[i] == '(') {
                    i++;
                }
                i = skip_balanced_parens(buf, i, len);
                if (i > len) {
                    return len;
                }
                continue;
            }
            if (ch == '$' && i + 1 < len && buf[i + 1] == '{') {
                i += 2;
                i = skip_braced_param(buf, i, len, dquote_context);
                continue;
            }
            if (ch == '$' && i + 1 < len && buf[i + 1] == '\'') {
                i = skip_dollar_single_quote(buf, i, len);
                if (i > len) {
                    return len;
                }
                continue;
            }
            if (ch == '`') {
                i = skip_backtick_subst(buf, i, len);
                continue;
            }
            if (ch == '"' || (!dquote_context && ch == '\'')) {
                quote = ch;
                i++;
                continue;
            }
            if (ch == '{') {
                depth++;
                i++;
                continue;
            }
            if (ch == '}') {
                depth--;
                i++;
                if (depth == 0) {
                    return i;
                }
                continue;
            }
            i++;
            continue;
        }

        if (quote == '\'' && ch == '\'') {
            quote = '\0';
            i++;
            continue;
        }
        if (quote == '"' && ch == '\\' && i + 1 < len) {
            i += 2;
            continue;
        }
        if (quote == '"' && ch == '"') {
            quote = '\0';
        }
        i++;
    }
    return len;
}

static void free_heredoc_markers(struct heredoc_marker *markers, size_t count) {
    size_t i;

    for (i = 0; i < count; i++) {
        arena_maybe_free(markers[i].delimiter);
    }
    arena_maybe_free(markers);
}

static void pending_heredoc_clear(struct pending_heredoc_state *state) {
    if (state == NULL) {
        return;
    }
    if (state->markers != NULL) {
        free_heredoc_markers(state->markers, state->marker_count);
    }
    state->markers = NULL;
    state->marker_count = 0;
    state->marker_index = 0;
    state->active = false;
}

static bool pending_heredoc_line_matches(const struct heredoc_marker *marker,
                                         const char *line, size_t len) {
    size_t cmp_start;
    size_t delim_len;

    cmp_start = 0;
    if (marker->strip_tabs) {
        while (cmp_start < len && line[cmp_start] == '\t') {
            cmp_start++;
        }
    }

    delim_len = strlen(marker->delimiter);
    return len >= cmp_start && len - cmp_start == delim_len &&
           strncmp(line + cmp_start, marker->delimiter, delim_len) == 0;
}

static void pending_heredoc_consume_line(struct pending_heredoc_state *state,
                                         const char *line, size_t len) {
    if (!state->active) {
        return;
    }
    if (state->marker_index >= state->marker_count) {
        state->active = false;
        return;
    }

    if (pending_heredoc_line_matches(&state->markers[state->marker_index], line,
                                     len)) {
        state->marker_index++;
        if (state->marker_index >= state->marker_count) {
            state->active = false;
        }
    }
}

static void pending_heredoc_consume_existing_body(
    struct pending_heredoc_state *state, const char *buf, size_t len) {
    size_t pos;

    if (!state->active) {
        return;
    }

    pos = 0;
    while (pos < len && buf[pos] != '\n') {
        pos++;
    }
    if (pos >= len) {
        return;
    }
    pos++;

    while (pos <= len && state->active) {
        size_t line_start;
        size_t line_end;

        line_start = pos;
        while (pos < len && buf[pos] != '\n') {
            pos++;
        }
        line_end = pos;
        pending_heredoc_consume_line(state, buf + line_start, line_end - line_start);
        if (pos >= len) {
            break;
        }
        pos++;
    }
}

/*
 * Initialize incremental heredoc tracking once syntax is complete without
 * considering heredoc bodies. This avoids full-buffer rescans while very long
 * heredoc bodies are being accumulated.
 */
static bool pending_heredoc_begin(struct pending_heredoc_state *state,
                                  const char *buf, size_t len) {
    struct heredoc_marker *markers;
    size_t marker_count;

    markers = NULL;
    marker_count = 0;
    pending_heredoc_clear(state);
    if (!collect_heredoc_markers(buf, len, &markers, &marker_count)) {
        return true;
    }
    if (marker_count == 0) {
        return false;
    }

    state->markers = markers;
    state->marker_count = marker_count;
    state->marker_index = 0;
    state->active = true;
    pending_heredoc_consume_existing_body(state, buf, len);
    return state->active;
}

static char *unquote_heredoc_delimiter(const char *raw, size_t len) {
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
            if (raw[i + 1] == '\n') {
                i += 2;
                continue;
            }
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

static size_t skip_line_continuations_limited(const char *buf, size_t pos,
                                              size_t len) {
    while (pos + 1 < len && buf[pos] == '\\' && buf[pos + 1] == '\n') {
        pos += 2;
    }
    return pos;
}

static bool collect_heredoc_markers(const char *buf, size_t len,
                                    struct heredoc_marker **markers_out,
                                    size_t *count_out) {
    size_t i;
    char quote;
    struct heredoc_marker *markers;
    size_t count;

    i = 0;
    quote = '\0';
    markers = NULL;
    count = 0;

    while (i < len) {
        char ch;

        ch = buf[i];
        if (quote == '\0') {
            if (ch == '\\' && i + 1 < len) {
                i += 2;
                continue;
            }
            if (ch == '$' && i + 1 < len && buf[i + 1] == '(') {
                i += 2;
                if (i < len && buf[i] == '(') {
                    i++;
                }
                i = skip_balanced_parens(buf, i, len);
                if (i > len) {
                    free_heredoc_markers(markers, count);
                    return false;
                }
                continue;
            }
            if (ch == '$' && i + 1 < len && buf[i + 1] == '{') {
                i += 2;
                i = skip_braced_param(buf, i, len, false);
                continue;
            }
            if (ch == '$' && i + 1 < len && buf[i + 1] == '\'') {
                i = skip_dollar_single_quote(buf, i, len);
                if (i > len) {
                    free_heredoc_markers(markers, count);
                    return false;
                }
                continue;
            }
            if (ch == '`') {
                i = skip_backtick_subst(buf, i, len);
                continue;
            }
            if (ch == '\'' || ch == '"') {
                quote = ch;
                i++;
                continue;
            }
            if (ch == '<') {
                size_t op_pos;
                bool strip_tabs;
                size_t start;
                size_t end;
                char *delim;
                struct heredoc_marker *grown;

                op_pos = skip_line_continuations_limited(buf, i + 1, len);
                if (op_pos >= len || buf[op_pos] != '<') {
                    i++;
                    continue;
                }

                i = op_pos + 1;
                i = skip_line_continuations_limited(buf, i, len);
                strip_tabs = false;
                if (i < len && buf[i] == '-') {
                    strip_tabs = true;
                    i++;
                }
                while (i < len) {
                    i = skip_line_continuations_limited(buf, i, len);
                    if (i < len && (buf[i] == ' ' || buf[i] == '\t')) {
                        i++;
                        continue;
                    }
                    break;
                }

                start = i;
                while (i < len) {
                    i = skip_line_continuations_limited(buf, i, len);
                    if (i >= len) {
                        break;
                    }
                    if (isspace((unsigned char)buf[i]) || buf[i] == ';' ||
                        buf[i] == '&' || buf[i] == '|' || buf[i] == '<' ||
                        buf[i] == '>') {
                        break;
                    }
                    if (buf[i] == '\\' && i + 1 < len) {
                        i += 2;
                        continue;
                    }
                    if (buf[i] == '\'' || buf[i] == '"') {
                        char q;

                        q = buf[i];
                        i++;
                        while (i < len && buf[i] != q) {
                            if (q == '"' && buf[i] == '\\' && i + 1 < len) {
                                i += 2;
                                continue;
                            }
                            i++;
                        }
                        if (i < len && buf[i] == q) {
                            i++;
                        }
                        continue;
                    }
                    i++;
                }
                end = i;
                if (end <= start) {
                    continue;
                }

                delim = unquote_heredoc_delimiter(buf + start, end - start);
                if (delim == NULL) {
                    free_heredoc_markers(markers, count);
                    return false;
                }

                grown = arena_xrealloc(markers, sizeof(*markers) * (count + 1));
                markers = grown;
                markers[count].delimiter = delim;
                markers[count].strip_tabs = strip_tabs;
                count++;
                continue;
            }
        } else if (quote == '\'' && ch == '\'') {
            quote = '\0';
        } else if (quote == '"') {
            if (ch == '\\' && i + 1 < len) {
                i += 2;
                continue;
            }
            if (ch == '"') {
                quote = '\0';
            }
        }

        i++;
    }

    *markers_out = markers;
    *count_out = count;
    return true;
}

static bool heredoc_needs_more_input(const char *buf, size_t len) {
    struct heredoc_marker *markers;
    size_t marker_count;
    size_t i;
    size_t pos;

    markers = NULL;
    marker_count = 0;
    if (!collect_heredoc_markers(buf, len, &markers, &marker_count)) {
        return true;
    }
    if (marker_count == 0) {
        return false;
    }

    pos = 0;
    while (pos < len && buf[pos] != '\n') {
        pos++;
    }
    if (pos >= len) {
        free_heredoc_markers(markers, marker_count);
        return true;
    }
    pos++;

    for (i = 0; i < marker_count; i++) {
        bool found;

        found = false;
        while (pos <= len) {
            size_t line_start;
            size_t line_end;
            size_t cmp_start;
            size_t delim_len;

            line_start = pos;
            while (pos < len && buf[pos] != '\n') {
                pos++;
            }
            line_end = pos;
            cmp_start = line_start;
            if (markers[i].strip_tabs) {
                while (cmp_start < line_end && buf[cmp_start] == '\t') {
                    cmp_start++;
                }
            }

            delim_len = strlen(markers[i].delimiter);
            if (line_end - cmp_start == delim_len &&
                strncmp(buf + cmp_start, markers[i].delimiter, delim_len) == 0) {
                found = true;
                if (pos < len) {
                    pos++;
                }
                break;
            }

            if (pos >= len) {
                break;
            }
            pos++;
        }

        if (!found) {
            free_heredoc_markers(markers, marker_count);
            return true;
        }
    }

    free_heredoc_markers(markers, marker_count);
    return false;
}

static bool keyword_boundary(char ch) {
    return ch == '\0' || isspace((unsigned char)ch) || ch == ';' ||
           ch == '&' || ch == '|' || ch == '(' || ch == ')' || ch == '{' ||
           ch == '}';
}

static bool hash_starts_comment(const char *source, size_t pos) {
    if (source[pos] != '#') {
        return false;
    }

    if (pos == 0) {
        return true;
    }
    return isspace((unsigned char)source[pos - 1]) || source[pos - 1] == ';' ||
           source[pos - 1] == '&' || source[pos - 1] == '|' ||
           source[pos - 1] == '(' || source[pos - 1] == ')' ||
           source[pos - 1] == '{' || source[pos - 1] == '}';
}

static bool line_has_comment_before(const char *buf, size_t line_start,
                                    size_t line_end, size_t *comment_pos_out) {
    size_t i;
    int quote;
    int param_depth;

    i = line_start;
    quote = 0;
    param_depth = 0;
    while (i < line_end) {
        char ch;

        ch = buf[i];
        if (quote == 0) {
            if (ch == '\\' && i + 1 < line_end) {
                i += 2;
                continue;
            }
            if (ch == '\'' || ch == '"') {
                quote = ch;
                i++;
                continue;
            }
            if (ch == '$' && i + 1 < line_end && buf[i + 1] == '{') {
                param_depth++;
                i += 2;
                continue;
            }
            if (ch == '}' && param_depth > 0) {
                param_depth--;
                i++;
                continue;
            }
            if (ch == '#' && param_depth == 0 && hash_starts_comment(buf, i)) {
                if (comment_pos_out != NULL) {
                    *comment_pos_out = i;
                }
                return true;
            }
            i++;
            continue;
        }

        if (quote == '\'' && ch == '\'') {
            quote = 0;
        } else if (quote == '"' && ch == '\\' && i + 1 < line_end) {
            i += 2;
            continue;
        } else if (quote == '"' && ch == '"') {
            quote = 0;
        }
        i++;
    }

    return false;
}

static bool trailing_backslash_newline_is_comment(const char *buf, size_t len) {
    size_t line_start;
    size_t comment_pos;

    if (len < 2 || buf[len - 2] != '\\' || buf[len - 1] != '\n') {
        return false;
    }

    line_start = len - 2;
    while (line_start > 0 && buf[line_start - 1] != '\n') {
        line_start--;
    }
    return line_has_comment_before(buf, line_start, len - 1, &comment_pos);
}

static size_t trim_end_ignoring_trailing_comment(const char *buf, size_t len) {
    size_t end;
    size_t line_start;
    size_t comment_pos;

    end = len;
    while (end > 0 && isspace((unsigned char)buf[end - 1])) {
        end--;
    }
    if (end == 0) {
        return 0;
    }

    line_start = end;
    while (line_start > 0 && buf[line_start - 1] != '\n') {
        line_start--;
    }
    if (line_has_comment_before(buf, line_start, end, &comment_pos)) {
        end = comment_pos;
        while (end > 0 && isspace((unsigned char)buf[end - 1])) {
            end--;
        }
    }
    return end;
}

static bool looks_like_function_header_only_input(const char *buf, size_t len) {
    size_t i;
    size_t out_len;
    char *collapsed;
    int quote;
    bool result;

    collapsed = arena_xmalloc(len + 1);

    i = 0;
    out_len = 0;
    quote = 0;
    while (i < len) {
        char ch;

        ch = buf[i];
        if (quote == 0) {
            if (ch == '\\' && i + 1 < len && buf[i + 1] == '\n') {
                i += 2;
                continue;
            }
            if (ch == '\\' && i + 1 < len) {
                collapsed[out_len++] = ch;
                collapsed[out_len++] = buf[i + 1];
                i += 2;
                continue;
            }
            if (ch == '#') {
                if (hash_starts_comment(buf, i)) {
                    while (i < len && buf[i] != '\n') {
                        i++;
                    }
                    continue;
                }
            }
            if (ch == '\'' || ch == '"') {
                quote = ch;
            }
            collapsed[out_len++] = ch;
            i++;
            continue;
        }

        if (quote == '\'' && ch == '\'') {
            quote = 0;
            collapsed[out_len++] = ch;
            i++;
            continue;
        }
        if (quote == '"' && ch == '\\' && i + 1 < len) {
            collapsed[out_len++] = ch;
            collapsed[out_len++] = buf[i + 1];
            i += 2;
            continue;
        }
        if (quote == '"' && ch == '"') {
            quote = 0;
        }
        collapsed[out_len++] = ch;
        i++;
    }
    collapsed[out_len] = '\0';

    i = 0;
    while (collapsed[i] != '\0' && isspace((unsigned char)collapsed[i])) {
        i++;
    }
    if (!(isalpha((unsigned char)collapsed[i]) || collapsed[i] == '_')) {
        arena_maybe_free(collapsed);
        return false;
    }
    i++;
    while (isalnum((unsigned char)collapsed[i]) || collapsed[i] == '_') {
        i++;
    }
    while (isspace((unsigned char)collapsed[i])) {
        i++;
    }
    if (collapsed[i] != '(') {
        arena_maybe_free(collapsed);
        return false;
    }
    i++;
    while (isspace((unsigned char)collapsed[i])) {
        i++;
    }
    if (collapsed[i] != ')') {
        arena_maybe_free(collapsed);
        return false;
    }
    i++;
    while (isspace((unsigned char)collapsed[i])) {
        i++;
    }
    result = collapsed[i] == '\0';
    arena_maybe_free(collapsed);
    return result;
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

static size_t skip_continuations_forward(const char *source, size_t pos) {
    while (source[pos] == '\\' && source[pos + 1] == '\n') {
        pos += 2;
    }
    return pos;
}

static int needs_more_input(char *buf, size_t *len, bool include_heredoc) {
    size_t i;
    int quote;
    int paren_depth;
    int brace_depth;
    int param_depth;
    int if_depth;
    int case_depth;
    int loop_depth;
    bool command_position;

    quote = 0;
    paren_depth = 0;
    brace_depth = 0;
    param_depth = 0;
    if_depth = 0;
    case_depth = 0;
    loop_depth = 0;
    command_position = true;
    i = 0;
    while (i < *len) {
        char ch;

        ch = buf[i];
        if (quote == 0) {
            if (ch == '\'') {
                quote = '\'';
                i++;
                continue;
            }
            if (ch == '"') {
                quote = '"';
                i++;
                continue;
            }
            if (ch == '\\' && i + 1 < *len && buf[i + 1] == '\n') {
                i += 2;
                continue;
            }
            if (ch == '\\' && i + 1 >= *len) {
                return 1;
            }
            if (ch == '\\' && i + 1 < *len) {
                i += 2;
                continue;
            }
            if (ch == '$' && i + 1 < *len) {
                size_t next;

                next = skip_line_continuations_limited(buf, i + 1, *len);
                if (next < *len && buf[next] == '\'') {
                    i = next - 1;
                    i = skip_dollar_single_quote(buf, i, *len);
                    if (i > *len) {
                        return 1;
                    }
                    command_position = false;
                    continue;
                }
                if (next < *len && buf[next] == '(') {
                    i = next + 1;
                    if (i < *len && buf[i] == '(') {
                        i++;
                    }
                    i = skip_balanced_parens(buf, i, *len);
                    if (i > *len) {
                        return 1;
                    }
                    command_position = false;
                    continue;
                }
                if (next < *len && buf[next] == '{') {
                    param_depth++;
                    command_position = false;
                    i = next + 1;
                    continue;
                }
            }
            if (ch == '}' && param_depth > 0) {
                param_depth--;
                command_position = false;
                i++;
                continue;
            }
            if (ch == '`') {
                i = skip_backtick_subst(buf, i, *len);
                command_position = false;
                continue;
            }
            if (ch == '#' && param_depth == 0 && paren_depth == 0 &&
                brace_depth == 0 && hash_starts_comment(buf, i)) {
                while (i < *len && buf[i] != '\n') {
                    i++;
                }
                continue;
            }
            if (isspace((unsigned char)ch)) {
                if (ch == '\n') {
                    command_position = true;
                }
                i++;
                continue;
            }
            if (ch == ';') {
                command_position = true;
                i++;
                continue;
            }
            if ((ch == '|' || ch == '&') && i + 1 < *len && buf[i + 1] == ch) {
                command_position = true;
                i += 2;
                continue;
            }
            if (ch == '|' || ch == '&') {
                command_position = true;
                i++;
                continue;
            }
            if (ch == '(') {
                paren_depth++;
                command_position = true;
                i++;
                continue;
            }
            if (ch == ')' && paren_depth > 0) {
                paren_depth--;
                command_position = false;
                i++;
                continue;
            }
            if (ch == '{') {
                bool opens_group;

                opens_group = command_position;
                if (!opens_group) {
                    size_t j;

                    j = i;
                    while (j > 0 && isspace((unsigned char)buf[j - 1])) {
                        j--;
                    }
                    if (j > 0 && buf[j - 1] == ')') {
                        opens_group = true;
                    }
                }

                if (opens_group) {
                    brace_depth++;
                    command_position = true;
                } else {
                    command_position = false;
                }
                i++;
                continue;
            }
            if (ch == '}') {
                if (command_position && brace_depth > 0) {
                    brace_depth--;
                }
                command_position = false;
                i++;
                continue;
            }
            if (isalpha((unsigned char)ch) || ch == '_') {
                size_t start;
                size_t j;
                size_t boundary;
                char keyword[16];
                size_t kwlen;

                start = i;
                j = i;
                kwlen = 0;
                while (j < *len) {
                    if (buf[j] == '\\' && j + 1 < *len && buf[j + 1] == '\n') {
                        j += 2;
                        continue;
                    }
                    if (!isalnum((unsigned char)buf[j]) && buf[j] != '_') {
                        break;
                    }
                    if (kwlen + 1 < sizeof(keyword)) {
                        keyword[kwlen] = buf[j];
                    }
                    kwlen++;
                    j++;
                }
                boundary = skip_continuations_forward(buf, j);
                if (word_starts_command_position(buf, start) &&
                    keyword_boundary(buf[boundary])) {
                    if (kwlen == 2 && strncmp(keyword, "if", 2) == 0) {
                        if_depth++;
                    } else if (kwlen == 2 && strncmp(keyword, "fi", 2) == 0 &&
                               if_depth > 0) {
                        if_depth--;
                    } else if (kwlen == 4 &&
                               strncmp(keyword, "case", 4) == 0) {
                        case_depth++;
                    } else if (kwlen == 4 &&
                               strncmp(keyword, "esac", 4) == 0 &&
                               case_depth > 0) {
                        case_depth--;
                    } else if (case_depth == 0) {
                        if ((kwlen == 5 &&
                             strncmp(keyword, "while", 5) == 0) ||
                            (kwlen == 5 &&
                             strncmp(keyword, "until", 5) == 0) ||
                            (kwlen == 3 &&
                             strncmp(keyword, "for", 3) == 0)) {
                            loop_depth++;
                        } else if (kwlen == 4 &&
                                   strncmp(keyword, "done", 4) == 0 &&
                                   loop_depth > 0 &&
                                   keyword_preceded_by_list_separator(buf, start)) {
                            loop_depth--;
                        }
                    }
                }

                command_position = false;
                i = j;
                continue;
            }
            command_position = false;
            i++;
            continue;
        }

        if (quote == '\'' && ch == '\'') {
            quote = 0;
            i++;
            continue;
        }
        if (quote == '"' && ch == '$' && i + 1 < *len && buf[i + 1] == '(') {
            i += 2;
            if (i < *len && buf[i] == '(') {
                i++;
            }
            i = skip_balanced_parens(buf, i, *len);
            if (i > *len) {
                return 1;
            }
            continue;
        }
        if (quote == '"' && ch == '$' && i + 1 < *len) {
            size_t next;

            next = skip_line_continuations_limited(buf, i + 1, *len);
            if (next < *len && buf[next] == '{') {
                i = next + 1;
                i = skip_braced_param(buf, i, *len, true);
                continue;
            }
        }
        if (quote == '"' && ch == '`') {
            i = skip_backtick_subst(buf, i, *len);
            continue;
        }
        if (quote == '"' && ch == '\\' && i + 1 < *len) {
            i += 2;
            continue;
        }
        if (quote == '"' && ch == '"') {
            quote = 0;
            i++;
            continue;
        }
        i++;
    }

    if (quote != 0) {
        return 1;
    }
    if (paren_depth > 0 || brace_depth > 0) {
        return 1;
    }
    if (param_depth > 0) {
        return 1;
    }
    if (if_depth > 0 || case_depth > 0 || loop_depth > 0) {
        return 1;
    }
    /*
     * Detect incomplete function headers like "name() #comment" so we do not
     * execute them prematurely. Keep this bounded to avoid expensive scans on
     * very large multiline buffers (notably long heredoc bodies).
     */
    if (*len <= 8192 && looks_like_function_header_only_input(buf, *len)) {
        return 1;
    }
    if (*len >= 2 && buf[*len - 2] == '\\' && buf[*len - 1] == '\n' &&
        !trailing_backslash_newline_is_comment(buf, *len)) {
        return 1;
    }
    {
        size_t end;

        end = trim_end_ignoring_trailing_comment(buf, *len);
        if (end > 0) {
            if (buf[end - 1] == '|') {
                return 1;
            }
            if (buf[end - 1] == '&' && end >= 2 && buf[end - 2] == '&') {
                return 1;
            }
        }
    }
    if (!include_heredoc) {
        return 0;
    }
    return heredoc_needs_more_input(buf, *len) ? 1 : 0;
}

int shell_needs_more_input_text(const char *buf, size_t len) {
    return shell_needs_more_input_text_mode(buf, len, true);
}

int shell_needs_more_input_text_mode(const char *buf, size_t len,
                                     bool include_heredoc) {
    char *tmp;
    size_t tmp_len;
    int rc;

    tmp = arena_xmalloc(len + 1);
    memcpy(tmp, buf, len);
    tmp[len] = '\0';
    tmp_len = len;
    rc = needs_more_input(tmp, &tmp_len, include_heredoc);
    arena_maybe_free(tmp);
    return rc;
}

bool shell_position_in_comment(const char *buf, size_t len, size_t pos) {
    size_t line_start;
    size_t i;
    size_t line_end;
    size_t comment_pos;

    if (pos >= len) {
        return false;
    }

    line_start = pos;
    while (line_start > 0 && buf[line_start - 1] != '\n') {
        line_start--;
    }

    line_end = pos;
    while (line_end < len && buf[line_end] != '\n') {
        line_end++;
    }

    comment_pos = line_end;
    if (!line_has_comment_before(buf, line_start, line_end, &comment_pos)) {
        return false;
    }
    if (comment_pos >= line_end) {
        return false;
    }

    for (i = comment_pos; i < line_end; i++) {
        if (i == pos) {
            return true;
        }
    }
    return false;
}

static void append_command(char **buf, size_t *len, size_t *cap,
                           const char *line) {
    size_t n;

    n = strlen(line);
    if (*len + n + 1 > *cap) {
        size_t new_cap;

        new_cap = *cap == 0 ? 128 : *cap;
        while (*len + n + 1 > new_cap) {
            new_cap *= 2;
        }
        *buf = arena_realloc_in(NULL, *buf, new_cap);
        *cap = new_cap;
    }

    memcpy(*buf + *len, line, n);
    *len += n;
    (*buf)[*len] = '\0';
}

static void maybe_release_large_command_buffer(char **buf, size_t *cap) {
    const size_t max_reuse_cap = 256u * 1024u;

    if (*buf != NULL && *cap > max_reuse_cap) {
        arena_maybe_free(*buf);
        *buf = NULL;
        *cap = 0;
    }
}

static bool merge_need_more_with_alias_preview(struct shell_state *state,
                                               const char *command,
                                               size_t command_len,
                                               bool need_more,
                                               bool include_heredoc) {
    struct arena *saved_arena;
    struct arena_mark tmp_mark;
    bool used_temp_arena;
    char *alias_preview;
    bool result;

    saved_arena = arena_get_current();

    /*
     * While collecting heredoc bodies, alias preview cannot change completion.
     * Skipping preview avoids quadratic rescans on very long heredoc inputs.
     */
    if (need_more && include_heredoc && strstr(command, "<<") != NULL) {
        return true;
    }

    used_temp_arena = saved_arena != &state->arena_cmd;
    if (used_temp_arena) {
        arena_mark_take(&state->arena_cmd, &tmp_mark);
        arena_set_current(&state->arena_cmd);
    }

    alias_preview = exec_alias_expand_preview(state, command);
    if (alias_preview != NULL) {
        size_t alias_len;
        bool alias_need_more;
        bool raw_trailing_backslash_nl;

        alias_len = strlen(alias_preview);
        raw_trailing_backslash_nl =
            command_len >= 2 &&
            command[command_len - 2] == '\\' &&
            command[command_len - 1] == '\n';

        if (strstr(alias_preview, "<<") != NULL) {
            alias_need_more =
                needs_more_input(alias_preview, &alias_len, include_heredoc) != 0;
        } else {
            alias_need_more = exec_alias_preview_needs_more(alias_preview);
        }

        arena_maybe_free(alias_preview);

        if (raw_trailing_backslash_nl) {
            /*
             * Preserve explicit physical line continuation. Alias preview can
             * request more input but must not force execution before the
             * continued physical line arrives.
             */
            result = need_more || alias_need_more;
            goto out;
        }
        /*
         * Alias substitution is part of command recognition, so completeness
         * should follow the aliased text when no physical continuation is open.
         */
        result = alias_need_more;
        goto out;
    }

    result = need_more;
out:
    if (used_temp_arena) {
        arena_mark_rewind(&state->arena_cmd, &tmp_mark);
        arena_set_current(saved_arena);
    }
    return result;
}

static bool inherited_ignore_locked(const struct shell_state *state, int signo) {
    return !state->interactive && signals_inherited_ignored(signo) &&
           !state->parent_was_interactive;
}

static bool trap_clear_keeps_ignore(const struct shell_state *state, int signo) {
    if (state->interactive && state->main_context && signals_policy_ignored(signo)) {
        return true;
    }

    return inherited_ignore_locked(state, signo);
}

static const char *trap_resolve_alias_command(const char *command) {
    size_t start;
    size_t end;
    size_t i;
    size_t name_len;
    size_t key_len;
    char *key;
    const char *value;

    start = 0;
    while (command[start] == ' ' || command[start] == '\t' ||
           command[start] == '\n') {
        start++;
    }
    end = strlen(command);
    while (end > start &&
           (command[end - 1] == ' ' || command[end - 1] == '\t' ||
            command[end - 1] == '\n')) {
        end--;
    }
    if (end == start) {
        return NULL;
    }

    if (!(isalpha((unsigned char)command[start]) || command[start] == '_')) {
        return NULL;
    }
    for (i = start + 1; i < end; i++) {
        if (!(isalnum((unsigned char)command[i]) || command[i] == '_')) {
            return NULL;
        }
    }

    name_len = end - start;
    key_len = strlen(POSISH_ALIAS_ENV_PREFIX) + name_len;
    key = arena_xmalloc(key_len + 1);
    memcpy(key, POSISH_ALIAS_ENV_PREFIX, strlen(POSISH_ALIAS_ENV_PREFIX));
    memcpy(key + strlen(POSISH_ALIAS_ENV_PREFIX), command + start, name_len);
    key[key_len] = '\0';
    value = getenv(key);
    arena_maybe_free(key);
    return value;
}

void shell_state_init(struct shell_state *state) {
    int signo;

    arena_init(&state->arena_perm, 256u * 1024u);
    arena_init(&state->arena_script, 512u * 1024u);
    arena_init(&state->arena_cmd, 256u * 1024u);
    arena_set_current(&state->arena_perm);

    state->last_status = 0;
    state->last_cmdsub_status = 0;
    state->cmdsub_performed = false;
    /* Keep $$ stable across subshell/cmdsub contexts for POSIX semantics. */
    state->shell_pid = getpid();
    state->prompt_command_index = 1;
    state->errexit = false;
    state->errexit_ignored = false;
    state->should_exit = false;
    state->exit_status = 0;
    state->last_handled_signal = 0;
    state->interactive = false;
    state->login_shell = false;
    state->explicit_non_interactive = false;
    state->parent_was_interactive = false;
    state->monitor_mode = false;
    state->allexport = false;
    state->notify = false;
    state->noclobber = false;
    state->noglob = false;
    state->hashondef = false;
    state->noexec = false;
    state->nounset = false;
    state->verbose = false;
    state->xtrace = false;
    state->pipefail = false;
    state->ignoreeof = false;
    state->in_async_context = false;
    state->main_context = true;
    state->in_command_builtin = false;
    state->last_async_pid = -1;
    state->break_levels = 0;
    state->continue_levels = 0;
    state->loop_depth = 0;
    state->return_requested = false;
    state->return_status = 0;
    state->function_depth = 0;
    state->dot_depth = 0;
    state->exit_trap = NULL;
    state->running_exit_trap = false;
    state->running_signal_trap = false;
    state->trap_entry_status_valid = false;
    state->trap_entry_status = 0;
    for (signo = 0; signo < NSIG; signo++) {
        state->signal_traps[signo] = NULL;
        state->signal_cleared[signo] = false;
    }
    state->readonly_names = NULL;
    state->readonly_count = 0;
    state->functions = NULL;
    state->function_count = 0;
    state->unexported_names = NULL;
    state->unexported_count = 0;
    state->positional_params = NULL;
    state->positional_count = 0;
    state->shell_name = NULL;
}

void shell_state_destroy(struct shell_state *state) {
    int signo;

    for (signo = 0; signo < NSIG; signo++) {
        state->signal_traps[signo] = NULL;
        state->signal_cleared[signo] = false;
    }

    state->readonly_names = NULL;
    state->readonly_count = 0;
    state->functions = NULL;
    state->function_count = 0;
    state->unexported_names = NULL;
    state->unexported_count = 0;
    state->positional_params = NULL;
    state->positional_count = 0;
    state->login_shell = false;
    state->explicit_non_interactive = false;
    state->parent_was_interactive = false;
    state->errexit_ignored = false;
    state->in_async_context = false;
    state->main_context = true;
    state->last_async_pid = -1;
    state->break_levels = 0;
    state->continue_levels = 0;
    state->loop_depth = 0;
    state->return_requested = false;
    state->return_status = 0;
    state->function_depth = 0;
    state->dot_depth = 0;
    state->exit_trap = NULL;
    state->running_exit_trap = false;
    state->running_signal_trap = false;
    state->last_handled_signal = 0;
    state->allexport = false;
    state->notify = false;
    state->noclobber = false;
    state->noglob = false;
    state->hashondef = false;
    state->noexec = false;
    state->nounset = false;
    state->verbose = false;
    state->xtrace = false;
    state->pipefail = false;
    state->ignoreeof = false;
    state->in_command_builtin = false;
    state->trap_entry_status_valid = false;
    state->trap_entry_status = 0;

    arena_set_current(NULL);
    arena_destroy(&state->arena_cmd);
    arena_destroy(&state->arena_script);
    arena_destroy(&state->arena_perm);
}

void shell_refresh_signal_policy(struct shell_state *state) {
    int signo;

    /*
     * Keep runtime option flips (set +/-i, set +/-m) and trap state in one
     * place so all execution paths observe the same live dispositions.
     */
    signals_apply_policy(state->interactive, state->monitor_mode);

    for (signo = 1; signo < NSIG; signo++) {
        if (state->signal_traps[signo] != NULL) {
            if (state->signal_traps[signo][0] == '\0') {
                (void)signals_set_ignored(signo);
            } else if (inherited_ignore_locked(state, signo)) {
                arena_maybe_free(state->signal_traps[signo]);
                state->signal_traps[signo] = NULL;
                (void)signals_set_ignored(signo);
            } else {
                (void)signals_set_trap(signo);
            }
        } else if (state->signal_cleared[signo]) {
            if (trap_clear_keeps_ignore(state, signo)) {
                (void)signals_set_ignored(signo);
            } else {
                (void)signals_set_default(signo);
            }
        }
        signals_clear_pending(signo);
    }
}

void shell_init_startup_env(struct shell_state *state, const char *argv0) {
    prompt_init_defaults(state, argv0);
}

static char *home_startup_path(const char *leaf) {
    const char *home;
    size_t home_len;
    size_t leaf_len;
    bool need_slash;
    char *path;

    home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
        return NULL;
    }

    home_len = strlen(home);
    leaf_len = strlen(leaf);
    need_slash = home[home_len - 1] != '/';
    path = arena_alloc_in(NULL, home_len + (need_slash ? 1u : 0u) + leaf_len + 1u);
    if (path == NULL) {
        return NULL;
    }

    memcpy(path, home, home_len);
    if (need_slash) {
        path[home_len++] = '/';
    }
    memcpy(path + home_len, leaf, leaf_len);
    path[home_len + leaf_len] = '\0';
    return path;
}

static int expand_startup_path(struct shell_state *state, const char *text,
                               char **out) {
    struct token_vec lexed;
    struct token_vec expanded;
    struct arena *saved_arena;
    struct arena_mark cmd_mark;
    int rc;

    *out = NULL;
    if (text == NULL || text[0] == '\0') {
        return 0;
    }

    saved_arena = arena_get_current();
    arena_mark_take(&state->arena_cmd, &cmd_mark);
    arena_set_current(&state->arena_cmd);

    rc = lexer_split_words(text, &lexed);
    if (rc != 0) {
        state->last_status = 2;
        goto done;
    }

    rc = expand_words(&lexed, &expanded, state, false);
    lexer_free_tokens(&lexed);
    if (rc != 0) {
        state->last_status = 2;
        goto done;
    }

    if (expanded.len == 0) {
        lexer_free_tokens(&expanded);
        rc = 0;
        goto done;
    }
    if (expanded.len != 1) {
        posish_errorf("ENV must expand to a single pathname");
        lexer_free_tokens(&expanded);
        state->last_status = 2;
        rc = 2;
        goto done;
    }

    *out = arena_xstrdup(expanded.items[0]);
    lexer_free_tokens(&expanded);
    rc = 0;

done:
    arena_mark_rewind(&state->arena_cmd, &cmd_mark);
    arena_set_current(saved_arena);
    return rc;
}

static int run_startup_path(struct shell_state *state, const char *path,
                            bool interactive) {
    if (path == NULL || path[0] == '\0') {
        return 0;
    }

    if (access(path, F_OK) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
            return 0;
        }
        perror(path);
        state->last_status = 126;
        return 126;
    }

    return shell_run_file_mode(state, path, interactive);
}

int shell_run_startup_files(struct shell_state *state) {
    int status;
    char *path;
    const char *env_value;
    const char *home;

    status = 0;
    home = getenv("HOME");

    if (state->login_shell) {
        path = home_startup_path(".posish_profile");
        if (path == NULL) {
            if (home == NULL || home[0] == '\0') {
                goto maybe_env;
            }
            posish_errorf("failed to build ~/.posish_profile path");
            state->last_status = 1;
            return 1;
        }
        status = run_startup_path(state, path, state->interactive);
        arena_maybe_free(path);
        if (state->should_exit) {
            return status;
        }
    }

maybe_env:
    if (state->interactive) {
        env_value = getenv("ENV");
        if (env_value != NULL && env_value[0] != '\0') {
            path = NULL;
            if (expand_startup_path(state, env_value, &path) != 0) {
                if (state->last_status != 0) {
                    status = state->last_status;
                }
            } else {
                int env_status;

                env_status = run_startup_path(state, path, true);
                if (env_status != 0) {
                    status = env_status;
                }
            }
            arena_maybe_free(path);
            if (state->should_exit) {
                return status;
            }
        }

        path = home_startup_path(".posishrc");
        if (path == NULL) {
            home = getenv("HOME");
            if (home == NULL || home[0] == '\0') {
                return status;
            }
            posish_errorf("failed to build ~/.posishrc path");
            state->last_status = 1;
            return 1;
        }
        {
            int rc_status;

            rc_status = run_startup_path(state, path, true);
            if (rc_status != 0) {
                status = rc_status;
            }
        }
        arena_maybe_free(path);
    }

    return status;
}

void shell_run_pending_traps(struct shell_state *state) {
    int signo;

    if (state->running_signal_trap || state->running_exit_trap) {
        return;
    }

    state->running_signal_trap = true;
    for (;;) {
        int saved_last_status;
        bool saved_should_exit;
        int saved_exit_status;
        bool saved_trap_status_valid;
        int saved_trap_status;
        char *command;
        const char *run_command;
        const char *alias_value;

        signo = signals_take_next_pending();
        if (signo <= 0 || signo >= NSIG) {
            break;
        }

        state->last_handled_signal = signo;
        command = state->signal_traps[signo];
        if (command == NULL || command[0] == '\0') {
            trace_log(POSISH_TRACE_TRAPS,
                      "pending signal=%d has no command trap", signo);
            continue;
        }

        trace_log(POSISH_TRACE_TRAPS, "run signal trap signo=%d command=%s",
                  signo, command);
        alias_value = trap_resolve_alias_command(command);
        run_command = alias_value != NULL ? alias_value : command;

        saved_last_status = state->last_status;
        saved_should_exit = state->should_exit;
        saved_exit_status = state->exit_status;
        saved_trap_status_valid = state->trap_entry_status_valid;
        saved_trap_status = state->trap_entry_status;

        state->should_exit = false;
        state->trap_entry_status = saved_last_status;
        state->trap_entry_status_valid = true;
        (void)shell_run_command(state, run_command);
        state->trap_entry_status_valid = saved_trap_status_valid;
        state->trap_entry_status = saved_trap_status;
        trace_log(POSISH_TRACE_TRAPS,
                  "signal trap signo=%d finished status=%d should_exit=%d",
                  signo, state->last_status, state->should_exit ? 1 : 0);
        if (!state->should_exit) {
            state->should_exit = saved_should_exit;
            state->exit_status = saved_exit_status;
        }
        state->last_status = saved_last_status;
    }
    state->running_signal_trap = false;
}

void shell_run_exit_trap(struct shell_state *state) {
    int saved_last_status;
    int saved_exit_status;
    bool saved_should_exit;
    bool saved_trap_status_valid;
    int saved_trap_status;

    if (state->running_exit_trap || state->exit_trap == NULL) {
        return;
    }

    saved_last_status = state->last_status;
    saved_should_exit = state->should_exit;
    saved_exit_status = state->exit_status;
    saved_trap_status_valid = state->trap_entry_status_valid;
    saved_trap_status = state->trap_entry_status;

    state->running_exit_trap = true;
    state->should_exit = false;
    state->trap_entry_status = saved_last_status;
    state->trap_entry_status_valid = true;
    trace_log(POSISH_TRACE_TRAPS, "run EXIT trap command=%s", state->exit_trap);
    (void)shell_run_command(state, state->exit_trap);
    state->trap_entry_status_valid = saved_trap_status_valid;
    state->trap_entry_status = saved_trap_status;
    state->running_exit_trap = false;

    /* Keep the original shell exit unless the trap explicitly requested one. */
    if (!state->should_exit) {
        state->should_exit = saved_should_exit;
        state->exit_status = saved_exit_status;
        state->last_status = saved_last_status;
    }
}

int shell_run_command(struct shell_state *state, const char *command) {
    struct ast_program *program;
    const char *p;
    const char *command_text;
    char *command_copy;
    int status;
    struct arena *saved_arena;
    struct arena *active_arena;
    bool top_level_command;
    struct arena_mark cmd_mark;
    struct arena_mark nested_mark;

    command_copy = arena_alloc_in(NULL, strlen(command) + 1);
    memcpy(command_copy, command, strlen(command) + 1);
    command_text = command_copy;

    p = command_text;
    while (*p == ' ' || *p == '\t' || *p == '\n') {
        p++;
    }
    if (*p == '\0') {
        arena_maybe_free(command_copy);
        shell_run_pending_traps(state);
        return state->last_status;
    }

    trace_log(POSISH_TRACE_TRAPS, "shell_run_command: %s", command_text);
    shell_run_pending_traps(state);

    saved_arena = arena_get_current();
    top_level_command =
        saved_arena != &state->arena_script && saved_arena != &state->arena_cmd;
    if (top_level_command) {
        arena_reset(&state->arena_script);
        arena_reset(&state->arena_cmd);
        active_arena = &state->arena_script;
        arena_set_current(active_arena);
    } else {
        active_arena = saved_arena;
        arena_mark_take(active_arena, &nested_mark);
        arena_set_current(active_arena);
    }

    if (parse_program(command_text, &program) != 0) {
        if (!top_level_command) {
            arena_mark_rewind(active_arena, &nested_mark);
        }
        arena_set_current(saved_arena);
        arena_maybe_free(command_copy);
        state->last_status = 2;
        if (!state->interactive) {
            state->should_exit = true;
            state->exit_status = 2;
        }
        return state->last_status;
    }

    if (top_level_command) {
        arena_mark_take(&state->arena_cmd, &cmd_mark);
        arena_set_current(&state->arena_cmd);
    } else {
        arena_set_current(active_arena);
    }
    status = exec_run_program(state, program);
    ast_program_free(program);
    if (top_level_command) {
        arena_mark_rewind(&state->arena_cmd, &cmd_mark);
        arena_set_current(&state->arena_script);
    } else {
        arena_mark_rewind(active_arena, &nested_mark);
        arena_set_current(active_arena);
    }
    arena_set_current(saved_arena);
    if (top_level_command) {
        arena_reset(&state->arena_cmd);
        arena_reset(&state->arena_script);
    }
    arena_maybe_free(command_copy);

    state->last_status = status;
    trace_log(POSISH_TRACE_TRAPS, "command finished status=%d", status);
    shell_run_pending_traps(state);
    return status;
}

int shell_run_stream(struct shell_state *state, FILE *stream, bool interactive) {
    char *line;
    char *command;
    size_t cap;
    size_t command_len;
    size_t command_cap;
    bool secondary_prompt;
    bool line_mode_input;
    size_t line_no;
    size_t command_start_line;
    struct pending_heredoc_state pending_heredoc;

    line = NULL;
    cap = 0;
    command = NULL;
    command_len = 0;
    command_cap = 0;
    secondary_prompt = false;
    line_mode_input =
        (interactive && isatty(fileno(stream))) ||
        (!interactive && state->monitor_mode &&
         !state->explicit_non_interactive);
    line_no = 1;
    command_start_line = 1;
    pending_heredoc.markers = NULL;
    pending_heredoc.marker_count = 0;
    pending_heredoc.marker_index = 0;
    pending_heredoc.active = false;
    state->interactive = interactive;

    if (line_mode_input && !interactive) {
        /*
         * Keep stdin position synchronized with child jobs in monitor-mode
         * test runs. Buffered stdio prefetch can otherwise consume future
         * script lines before background commands read from fd 0.
         */
        (void)setvbuf(stream, NULL, _IONBF, 0);
    }

    if (!line_mode_input) {
        ssize_t nread;
        bool ran_command;

        ran_command = false;
        /*
         * Consume one complete top-level command at a time so child commands
         * can continue reading from stdin between command boundaries.
         */
        (void)setvbuf(stream, NULL, _IONBF, 0);
        while (!state->should_exit && (nread = getline(&line, &cap, stream)) >= 0) {
            char base_buf[32];

            (void)nread;
            if (state->verbose) {
                fputs(line, stderr);
            }
            if (command_len == 0) {
                command_start_line = line_no;
            }
            append_command(&command, &command_len, &command_cap, line);
            if (pending_heredoc.active) {
                size_t line_len;

                line_len = strlen(line);
                if (line_len > 0 && line[line_len - 1] == '\n') {
                    line_len--;
                }
                pending_heredoc_consume_line(&pending_heredoc, line, line_len);
                if (pending_heredoc.active) {
                    line_no++;
                    continue;
                }
            } else {
                bool need_more;

                need_more = needs_more_input(command, &command_len, false);
                if (!need_more) {
                    need_more =
                        pending_heredoc_begin(&pending_heredoc, command, command_len);
                }
                need_more = merge_need_more_with_alias_preview(
                    state, command, command_len, need_more, true);
                if (need_more) {
                    line_no++;
                    continue;
                }
            }

            if (pending_heredoc.active) {
                line_no++;
                continue;
            }

            snprintf(base_buf, sizeof(base_buf), "%zu", command_start_line - 1);
            (void)setenv("POSISH_LINENO_BASE", base_buf, 1);
            shell_run_command(state, command);
            pending_heredoc_clear(&pending_heredoc);
            command_len = 0;
            if (command != NULL) {
                command[0] = '\0';
            }
            maybe_release_large_command_buffer(&command, &command_cap);
            ran_command = true;
            if (state->return_requested && state->dot_depth > 0) {
                break;
            }
            if (state->should_exit) {
                break;
            }
            line_no++;
        }
        if (command_len > 0) {
            bool need_more;

            if (pending_heredoc.active) {
                need_more = true;
            } else {
                need_more = needs_more_input(command, &command_len, false);
                if (!need_more) {
                    need_more =
                        pending_heredoc_begin(&pending_heredoc, command, command_len);
                }
                need_more = merge_need_more_with_alias_preview(
                    state, command, command_len, need_more, true);
            }
            if (need_more) {
                posish_error_at("<input>", line_no, 1, "syntax",
                                "unexpected EOF while looking for matching quote");
                state->last_status = 2;
            } else {
                char base_buf[32];

                snprintf(base_buf, sizeof(base_buf), "%zu", command_start_line - 1);
                (void)setenv("POSISH_LINENO_BASE", base_buf, 1);
                shell_run_command(state, command);
                pending_heredoc_clear(&pending_heredoc);
                ran_command = true;
            }
        }
        shell_run_pending_traps(state);
        if (!ran_command && !state->should_exit) {
            /* Executing an empty script resets $? to success. */
            state->last_status = 0;
        }

        arena_maybe_free(line);
        arena_maybe_free(command);
        pending_heredoc_clear(&pending_heredoc);
        if (state->should_exit) {
            return state->exit_status;
        }
        return state->last_status;
    }

    while (!state->should_exit) {
        ssize_t nread;

        shell_run_pending_traps(state);
        if (state->should_exit) {
            break;
        }

        if (line_mode_input && isatty(STDOUT_FILENO)) {
            char *prompt_text;

            prompt_text = NULL;
            if (prompt_render(state, secondary_prompt ? "PS2" : "PS1",
                              &prompt_text) != 0) {
                fputs(secondary_prompt ? "> " : "$ ", stdout);
            } else {
                fputs(prompt_text == NULL ? "" : prompt_text, stdout);
            }
            arena_maybe_free(prompt_text);
            fflush(stdout);
        }

        nread = getline(&line, &cap, stream);
        if (nread < 0) {
            break;
        }
        if (state->verbose) {
            fputs(line, stderr);
        }

        if (command_len == 0) {
            command_start_line = line_no;
        }
        append_command(&command, &command_len, &command_cap, line);
        if (pending_heredoc.active) {
            size_t line_len;

            line_len = strlen(line);
            if (line_len > 0 && line[line_len - 1] == '\n') {
                line_len--;
            }
            pending_heredoc_consume_line(&pending_heredoc, line, line_len);
            if (pending_heredoc.active) {
                secondary_prompt = true;
                line_no++;
                continue;
            }
        } else {
            bool need_more;

            need_more = needs_more_input(command, &command_len, false);
            if (!need_more) {
                need_more =
                    pending_heredoc_begin(&pending_heredoc, command, command_len);
            }
            if (need_more) {
                secondary_prompt = true;
                line_no++;
                continue;
            }
        }

        {
            char base_buf[32];

            snprintf(base_buf, sizeof(base_buf), "%zu", command_start_line - 1);
            (void)setenv("POSISH_LINENO_BASE", base_buf, 1);
        }
        shell_run_command(state, command);
        state->prompt_command_index++;
        pending_heredoc_clear(&pending_heredoc);
        command_len = 0;
        if (command != NULL) {
            command[0] = '\0';
        }
        maybe_release_large_command_buffer(&command, &command_cap);
        secondary_prompt = false;
        if (state->return_requested && state->dot_depth > 0) {
            break;
        }
        line_no++;
    }

    if (command_len > 0) {
        bool need_more;

        if (pending_heredoc.active) {
            need_more = true;
        } else {
            need_more = needs_more_input(command, &command_len, false);
            if (!need_more) {
                need_more =
                    pending_heredoc_begin(&pending_heredoc, command, command_len);
            }
        }
        if (need_more) {
        posish_error_at("<input>", line_no, 1, "syntax",
                        "unexpected EOF while looking for matching quote");
        state->last_status = 2;
        }
    }

    arena_maybe_free(line);
    arena_maybe_free(command);
    pending_heredoc_clear(&pending_heredoc);
    (void)unsetenv("POSISH_LINENO_BASE");

    if (state->should_exit) {
        return state->exit_status;
    }

    return state->last_status;
}

int shell_run_file_mode(struct shell_state *state, const char *path,
                        bool interactive) {
    FILE *fp;
    int status;
    int open_errno;
    bool saved_interactive;

    fp = fopen(path, "r");
    if (fp == NULL) {
        open_errno = errno;
        perror(path);
        if (open_errno == ENOENT) {
            return 127;
        }
        return 126;
    }

    saved_interactive = state->interactive;
    status = shell_run_stream(state, fp, interactive);
    state->interactive = saved_interactive;
    fclose(fp);
    return status;
}

int shell_run_file(struct shell_state *state, const char *path) {
    return shell_run_file_mode(state, path, false);
}
