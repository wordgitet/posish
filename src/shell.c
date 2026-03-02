/* SPDX-License-Identifier: 0BSD */

/* posish - shell runtime */

#define _POSIX_C_SOURCE 200809L

#include "shell.h"

#include "ast.h"
#include "error.h"
#include "exec.h"
#include "parser.h"
#include "signals.h"
#include "trace.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct heredoc_marker {
    char *delimiter;
    bool strip_tabs;
};

static size_t skip_braced_param(const char *buf, size_t i, size_t len,
                                bool dquote_context);

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
        free(markers[i].delimiter);
    }
    free(markers);
}

static char *unquote_heredoc_delimiter(const char *raw, size_t len) {
    size_t i;
    size_t out_len;
    char *out;

    out = malloc(len + 1);
    if (out == NULL) {
        return NULL;
    }

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

                grown = realloc(markers, sizeof(*markers) * (count + 1));
                if (grown == NULL) {
                    free(delim);
                    free_heredoc_markers(markers, count);
                    return false;
                }
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

    collapsed = malloc(len + 1);
    if (collapsed == NULL) {
        return false;
    }

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
        free(collapsed);
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
        free(collapsed);
        return false;
    }
    i++;
    while (isspace((unsigned char)collapsed[i])) {
        i++;
    }
    if (collapsed[i] != ')') {
        free(collapsed);
        return false;
    }
    i++;
    while (isspace((unsigned char)collapsed[i])) {
        i++;
    }
    result = collapsed[i] == '\0';
    free(collapsed);
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

static int needs_more_input(char *buf, size_t *len) {
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
    if (looks_like_function_header_only_input(buf, *len)) {
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
    return heredoc_needs_more_input(buf, *len) ? 1 : 0;
}

int shell_needs_more_input_text(const char *buf, size_t len) {
    char *tmp;
    size_t tmp_len;
    int rc;

    tmp = malloc(len + 1);
    if (tmp == NULL) {
        perror("malloc");
        return 1;
    }
    memcpy(tmp, buf, len);
    tmp[len] = '\0';
    tmp_len = len;
    rc = needs_more_input(tmp, &tmp_len);
    free(tmp);
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
        *buf = realloc(*buf, new_cap);
        *cap = new_cap;
    }

    memcpy(*buf + *len, line, n);
    *len += n;
    (*buf)[*len] = '\0';
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

void shell_state_init(struct shell_state *state) {
    int signo;

    state->last_status = 0;
    state->last_cmdsub_status = 0;
    state->cmdsub_performed = false;
    /* Keep $$ stable across subshell/cmdsub contexts for POSIX semantics. */
    state->shell_pid = getpid();
    state->errexit = false;
    state->errexit_ignored = false;
    state->should_exit = false;
    state->exit_status = 0;
    state->last_handled_signal = 0;
    state->interactive = false;
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
}

void shell_state_destroy(struct shell_state *state) {
    size_t i;
    int signo;

    for (i = 0; i < state->readonly_count; i++) {
        free(state->readonly_names[i]);
    }
    free(state->readonly_names);

    for (i = 0; i < state->function_count; i++) {
        free(state->functions[i].name);
        free(state->functions[i].body);
    }
    free(state->functions);

    for (i = 0; i < state->unexported_count; i++) {
        free(state->unexported_names[i]);
    }
    free(state->unexported_names);

    for (i = 0; i < state->positional_count; i++) {
        free(state->positional_params[i]);
    }
    free(state->positional_params);
    free(state->exit_trap);
    for (signo = 0; signo < NSIG; signo++) {
        free(state->signal_traps[signo]);
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
                free(state->signal_traps[signo]);
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

        saved_last_status = state->last_status;
        saved_should_exit = state->should_exit;
        saved_exit_status = state->exit_status;
        saved_trap_status_valid = state->trap_entry_status_valid;
        saved_trap_status = state->trap_entry_status;

        state->should_exit = false;
        state->trap_entry_status = saved_last_status;
        state->trap_entry_status_valid = true;
        (void)shell_run_command(state, command);
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
    int status;

    p = command;
    while (*p == ' ' || *p == '\t' || *p == '\n') {
        p++;
    }
    if (*p == '\0') {
        shell_run_pending_traps(state);
        return state->last_status;
    }

    trace_log(POSISH_TRACE_TRAPS, "shell_run_command: %s", command);
    shell_run_pending_traps(state);

    if (parse_program(command, &program) != 0) {
        state->last_status = 2;
        return state->last_status;
    }

    status = exec_run_program(state, program);
    ast_program_free(program);

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
            if (needs_more_input(command, &command_len)) {
                line_no++;
                continue;
            }

            snprintf(base_buf, sizeof(base_buf), "%zu", command_start_line - 1);
            (void)setenv("POSISH_LINENO_BASE", base_buf, 1);
            shell_run_command(state, command);
            command_len = 0;
            if (command != NULL) {
                command[0] = '\0';
            }
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
            if (needs_more_input(command, &command_len)) {
                posish_error_at("<input>", line_no, 1, "syntax",
                                "unexpected EOF while looking for matching quote");
                state->last_status = 2;
            } else {
                char base_buf[32];

                snprintf(base_buf, sizeof(base_buf), "%zu", command_start_line - 1);
                (void)setenv("POSISH_LINENO_BASE", base_buf, 1);
                shell_run_command(state, command);
                ran_command = true;
            }
        }
        shell_run_pending_traps(state);
        if (!ran_command && !state->should_exit) {
            /* Executing an empty script resets $? to success. */
            state->last_status = 0;
        }

        free(line);
        free(command);
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
            fputs(secondary_prompt ? "> " : "posish$ ", stdout);
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
        if (needs_more_input(command, &command_len)) {
            secondary_prompt = true;
            line_no++;
            continue;
        }

        {
            char base_buf[32];

            snprintf(base_buf, sizeof(base_buf), "%zu", command_start_line - 1);
            (void)setenv("POSISH_LINENO_BASE", base_buf, 1);
        }
        shell_run_command(state, command);
        command_len = 0;
        if (command != NULL) {
            command[0] = '\0';
        }
        secondary_prompt = false;
        if (state->return_requested && state->dot_depth > 0) {
            break;
        }
        line_no++;
    }

    if (command_len > 0 && needs_more_input(command, &command_len)) {
        posish_error_at("<input>", line_no, 1, "syntax",
                        "unexpected EOF while looking for matching quote");
        state->last_status = 2;
    }

    free(line);
    free(command);
    (void)unsetenv("POSISH_LINENO_BASE");

    if (state->should_exit) {
        return state->exit_status;
    }

    return state->last_status;
}

int shell_run_file(struct shell_state *state, const char *path) {
    FILE *fp;
    int status;
    int open_errno;

    fp = fopen(path, "r");
    if (fp == NULL) {
        open_errno = errno;
        perror(path);
        if (open_errno == ENOENT) {
            return 127;
        }
        return 126;
    }

    status = shell_run_stream(state, fp, false);
    fclose(fp);
    return status;
}
