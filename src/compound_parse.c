/* SPDX-License-Identifier: 0BSD */

/* posish - compound command parsing */

#include "compound_parse.h"

#include "arena.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

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

static bool looks_like_redirection_suffix(const char *source, size_t pos) {
    size_t i;

    while (isspace((unsigned char)source[pos])) {
        pos++;
    }
    if (source[pos] == '\0') {
        return true;
    }

    i = pos;
    while (isdigit((unsigned char)source[i])) {
        i++;
    }
    if (source[i] != '<' && source[i] != '>') {
        return false;
    }

    for (; source[i] != '\0'; i++) {
        if (source[i] == '|' || source[i] == ';') {
            return false;
        }
    }
    return true;
}

bool parse_simple_if(const char *source, char **cond_out, char **then_out,
                     char **else_out, char **redir_out) {
    size_t i;
    size_t cond_start;
    size_t cond_end;
    size_t then_start;
    size_t then_end;
    size_t else_start;
    size_t fi_start;
    char quote;
    int paren_depth;
    int brace_depth;
    int if_depth;
    int case_depth;
    int loop_depth;

    i = 0;
    while (isspace((unsigned char)source[i])) {
        i++;
    }
    if (strncmp(source + i, "if", 2) != 0 || !keyword_boundary(source[i + 2])) {
        return false;
    }
    i += 2;
    while (isspace((unsigned char)source[i])) {
        i++;
    }

    cond_start = i;
    cond_end = 0;
    then_start = 0;
    quote = '\0';
    paren_depth = 0;
    brace_depth = 0;
    if_depth = 0;
    case_depth = 0;
    loop_depth = 0;
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
            if (paren_depth == 0 && brace_depth == 0 && if_depth == 0 &&
                case_depth == 0 && loop_depth == 0 &&
                strncmp(source + i, "then", 4) == 0 &&
                keyword_boundary(source[i + 4])) {
                cond_end = i;
                then_start = i + 4;
                break;
            }
            if (paren_depth == 0 && brace_depth == 0 &&
                (isalpha((unsigned char)ch) || ch == '_') &&
                word_starts_command_position(source, i)) {
                size_t j;
                size_t wlen;

                j = i;
                while (isalnum((unsigned char)source[j]) || source[j] == '_') {
                    j++;
                }
                if (keyword_boundary(source[j])) {
                    wlen = j - i;
                    if (wlen == 2 && strncmp(source + i, "if", 2) == 0) {
                        if_depth++;
                    } else if (wlen == 2 && strncmp(source + i, "fi", 2) == 0 &&
                               if_depth > 0) {
                        if_depth--;
                    } else if (wlen == 4 &&
                               strncmp(source + i, "case", 4) == 0) {
                        case_depth++;
                    } else if (wlen == 4 &&
                               strncmp(source + i, "esac", 4) == 0 &&
                               case_depth > 0) {
                        case_depth--;
                    } else if (case_depth == 0) {
                        if ((wlen == 5 &&
                             strncmp(source + i, "while", 5) == 0) ||
                            (wlen == 5 &&
                             strncmp(source + i, "until", 5) == 0) ||
                            (wlen == 3 && strncmp(source + i, "for", 3) == 0)) {
                            loop_depth++;
                        } else if (wlen == 4 &&
                                   strncmp(source + i, "done", 4) == 0 &&
                                   loop_depth > 0) {
                            loop_depth--;
                        } else if (wlen == 4 &&
                                   strncmp(source + i, "then", 4) == 0 &&
                                   if_depth == 0 && loop_depth == 0) {
                            cond_end = i;
                            then_start = j;
                            break;
                        }
                    }
                }
                i = j - 1;
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
    if (cond_end == 0) {
        return false;
    }

    while (isspace((unsigned char)source[then_start])) {
        then_start++;
    }
    then_end = 0;
    else_start = 0;
    fi_start = 0;
    quote = '\0';
    paren_depth = 0;
    brace_depth = 0;
    if_depth = 1;
    case_depth = 0;
    loop_depth = 0;
    for (i = then_start; source[i] != '\0'; i++) {
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
            if (paren_depth == 0 && brace_depth == 0 &&
                (isalpha((unsigned char)ch) || ch == '_') &&
                word_starts_command_position(source, i)) {
                size_t j;
                size_t wlen;

                j = i;
                while (isalnum((unsigned char)source[j]) || source[j] == '_') {
                    j++;
                }
                if (keyword_boundary(source[j])) {
                    wlen = j - i;
                    if (wlen == 2 && strncmp(source + i, "if", 2) == 0) {
                        if_depth++;
                    } else if (wlen == 4 &&
                               strncmp(source + i, "case", 4) == 0) {
                        case_depth++;
                    } else if (wlen == 4 &&
                               strncmp(source + i, "esac", 4) == 0 &&
                               case_depth > 0) {
                        case_depth--;
                    } else if (case_depth == 0 &&
                               ((wlen == 5 &&
                                 strncmp(source + i, "while", 5) == 0) ||
                                (wlen == 5 &&
                                 strncmp(source + i, "until", 5) == 0) ||
                                (wlen == 3 &&
                                 strncmp(source + i, "for", 3) == 0))) {
                        loop_depth++;
                    } else if (case_depth == 0 && wlen == 4 &&
                               strncmp(source + i, "done", 4) == 0 &&
                               loop_depth > 0) {
                        loop_depth--;
                    } else if (wlen == 2 && strncmp(source + i, "fi", 2) == 0) {
                        if_depth--;
                        if (if_depth == 0) {
                            size_t tail;

                            if (then_end == 0) {
                                then_end = i;
                            }
                            fi_start = i;
                            tail = j;
                            while (isspace((unsigned char)source[tail])) {
                                tail++;
                            }
                            if (!looks_like_redirection_suffix(source, tail)) {
                                return false;
                            }
                            if (redir_out != NULL) {
                                *redir_out =
                                    dup_trimmed_slice(source, tail, strlen(source));
                            }
                            break;
                        }
                    } else if (if_depth == 1 && then_end == 0 && case_depth == 0 &&
                               loop_depth == 0 &&
                               ((wlen == 4 && strncmp(source + i, "else", 4) == 0) ||
                                (wlen == 4 && strncmp(source + i, "elif", 4) == 0))) {
                        then_end = i;
                        /*
                         * Preserve `elif ...` by slicing from the keyword and
                         * rewriting it to `if ...` below.
                         */
                        else_start = i;
                    }
                }
                i = j - 1;
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

    if (then_end == 0 || fi_start == 0) {
        return false;
    }
    *cond_out = dup_trimmed_slice(source, cond_start, cond_end);
    *then_out = dup_trimmed_slice(source, then_start, then_end);
    if (else_start != 0) {
        char *else_body;

        else_body = dup_trimmed_slice(source, else_start, fi_start);
        if (strncmp(else_body, "elif", 4) == 0 && keyword_boundary(else_body[4])) {
            size_t len;
            size_t tail_len;
            char *rewritten;

            len = strlen(else_body);
            tail_len = len - 4;
            rewritten = arena_xmalloc(2 + tail_len + 3 + 1);
            memcpy(rewritten, "if", 2);
            memcpy(rewritten + 2, else_body + 4, tail_len);
            memcpy(rewritten + 2 + tail_len, "\nfi", 4);
            arena_maybe_free(else_body);
            else_body = rewritten;
        } else if (strncmp(else_body, "else", 4) == 0 &&
                   keyword_boundary(else_body[4])) {
            char *trimmed_else;
            size_t j;

            j = 4;
            while (isspace((unsigned char)else_body[j])) {
                j++;
            }
            trimmed_else = arena_xstrdup(else_body + j);
            arena_maybe_free(else_body);
            else_body = trimmed_else;
        }
        *else_out = else_body;
    } else {
        *else_out = NULL;
    }
    return true;
}

bool parse_simple_while(const char *source, char **cond_out, char **body_out,
                        bool *is_until_out, char **redir_out) {
    size_t i;
    size_t cond_start;
    size_t cond_end;
    size_t body_start;
    size_t body_end;
    char quote;
    int paren_depth;
    int brace_depth;
    int case_depth;
    int loop_depth;
    bool is_until;

    i = 0;
    while (isspace((unsigned char)source[i])) {
        i++;
    }
    if (strncmp(source + i, "while", 5) == 0 && keyword_boundary(source[i + 5])) {
        is_until = false;
        i += 5;
    } else if (strncmp(source + i, "until", 5) == 0 &&
               keyword_boundary(source[i + 5])) {
        is_until = true;
        i += 5;
    } else {
        return false;
    }
    while (isspace((unsigned char)source[i])) {
        i++;
    }

    cond_start = i;
    cond_end = 0;
    body_start = 0;
    quote = '\0';
    paren_depth = 0;
    brace_depth = 0;
    case_depth = 0;
    loop_depth = 0;

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
            if (paren_depth == 0 && brace_depth == 0 &&
                (isalpha((unsigned char)ch) || ch == '_') &&
                word_starts_command_position(source, i)) {
                size_t j;
                size_t wlen;

                j = i;
                while (isalnum((unsigned char)source[j]) || source[j] == '_') {
                    j++;
                }
                if (keyword_boundary(source[j])) {
                    wlen = j - i;
                    if (wlen == 4 && strncmp(source + i, "case", 4) == 0) {
                        case_depth++;
                    } else if (wlen == 4 &&
                               strncmp(source + i, "esac", 4) == 0 &&
                               case_depth > 0) {
                        case_depth--;
                    } else if (case_depth == 0) {
                        if ((wlen == 5 &&
                             strncmp(source + i, "while", 5) == 0) ||
                            (wlen == 5 &&
                             strncmp(source + i, "until", 5) == 0) ||
                            (wlen == 3 && strncmp(source + i, "for", 3) == 0)) {
                            loop_depth++;
                        } else if (wlen == 2 &&
                                   strncmp(source + i, "do", 2) == 0 &&
                                   loop_depth == 0) {
                            cond_end = i;
                            body_start = j;
                            break;
                        } else if (wlen == 4 &&
                                   strncmp(source + i, "done", 4) == 0 &&
                                   loop_depth > 0) {
                            loop_depth--;
                        }
                    }
                }
                i = j - 1;
                continue;
            }
            if (paren_depth == 0 && brace_depth == 0 && case_depth == 0 &&
                loop_depth == 0 && (ch == ';' || ch == '\n')) {
                size_t j;

                j = i + 1;
                while (isspace((unsigned char)source[j])) {
                    j++;
                }
                if (strncmp(source + j, "do", 2) == 0 &&
                    keyword_boundary(source[j + 2])) {
                    cond_end = i;
                    body_start = j + 2;
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

    if (cond_end == 0) {
        return false;
    }

    while (isspace((unsigned char)source[body_start])) {
        body_start++;
    }

    quote = '\0';
    paren_depth = 0;
    brace_depth = 0;
    case_depth = 0;
    loop_depth = 1;
    body_end = 0;

    for (i = body_start; source[i] != '\0'; i++) {
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
            if (paren_depth == 0 && brace_depth == 0 &&
                (isalpha((unsigned char)ch) || ch == '_') &&
                word_starts_command_position(source, i)) {
                size_t j;
                size_t wlen;

                j = i;
                while (isalnum((unsigned char)source[j]) || source[j] == '_') {
                    j++;
                }
                if (keyword_boundary(source[j])) {
                    wlen = j - i;
                    if (wlen == 4 && strncmp(source + i, "case", 4) == 0) {
                        case_depth++;
                    } else if (wlen == 4 &&
                               strncmp(source + i, "esac", 4) == 0 &&
                               case_depth > 0) {
                        case_depth--;
                    } else if (case_depth == 0) {
                        if ((wlen == 5 &&
                             strncmp(source + i, "while", 5) == 0) ||
                            (wlen == 5 &&
                             strncmp(source + i, "until", 5) == 0) ||
                            (wlen == 3 && strncmp(source + i, "for", 3) == 0)) {
                            loop_depth++;
                        } else if (wlen == 4 &&
                                   strncmp(source + i, "done", 4) == 0) {
                            loop_depth--;
                            if (loop_depth == 0) {
                                size_t tail;

                                body_end = i;
                                tail = j;
                                while (isspace((unsigned char)source[tail])) {
                                    tail++;
                                }
                                if (!looks_like_redirection_suffix(source, tail)) {
                                    return false;
                                }

                                *cond_out =
                                    dup_trimmed_slice(source, cond_start, cond_end);
                                *body_out =
                                    dup_trimmed_slice(source, body_start, body_end);
                                *is_until_out = is_until;
                                if (redir_out != NULL) {
                                    *redir_out =
                                        dup_trimmed_slice(source, tail, strlen(source));
                                }
                                return true;
                            }
                        }
                    }
                }
                i = j - 1;
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

    return false;
}

bool parse_simple_for(const char *source, char **name_out, char **words_out,
                      char **body_out, bool *implicit_words_out,
                      char **redir_out) {
    size_t i;
    size_t name_start;
    size_t name_end;
    size_t words_start;
    size_t words_end;
    size_t body_start;
    size_t body_end;
    char quote;
    int paren_depth;
    int brace_depth;
    int case_depth;
    int loop_depth;
    bool implicit_words;

    i = 0;
    while (isspace((unsigned char)source[i])) {
        i++;
    }
    if (strncmp(source + i, "for", 3) != 0 || !keyword_boundary(source[i + 3])) {
        return false;
    }
    i += 3;
    while (isspace((unsigned char)source[i])) {
        i++;
    }

    if (!(isalpha((unsigned char)source[i]) || source[i] == '_')) {
        return false;
    }
    name_start = i;
    i++;
    while (isalnum((unsigned char)source[i]) || source[i] == '_') {
        i++;
    }
    name_end = i;

    while (isspace((unsigned char)source[i])) {
        i++;
    }
    implicit_words = true;
    words_start = i;
    words_end = i;
    if (strncmp(source + i, "in", 2) == 0 && keyword_boundary(source[i + 2])) {
        implicit_words = false;
        i += 2;
        while (isspace((unsigned char)source[i])) {
            i++;
        }
        words_start = i;
        words_end = 0;
    } else {
        while (isspace((unsigned char)source[i])) {
            i++;
        }
        if (source[i] == ';') {
            i++;
        }
        while (isspace((unsigned char)source[i])) {
            i++;
        }
        if (strncmp(source + i, "do", 2) != 0 || !keyword_boundary(source[i + 2])) {
            return false;
        }
        words_end = words_start;
        body_start = i + 2;
        goto have_body_start;
    }

    body_start = 0;
    quote = '\0';
    paren_depth = 0;
    brace_depth = 0;
    case_depth = 0;
    loop_depth = 0;

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
            if (paren_depth == 0 && brace_depth == 0 &&
                (isalpha((unsigned char)ch) || ch == '_') &&
                word_starts_command_position(source, i)) {
                size_t j;
                size_t wlen;

                j = i;
                while (isalnum((unsigned char)source[j]) || source[j] == '_') {
                    j++;
                }
                if (keyword_boundary(source[j])) {
                    wlen = j - i;
                    if (wlen == 4 && strncmp(source + i, "case", 4) == 0) {
                        case_depth++;
                    } else if (wlen == 4 &&
                               strncmp(source + i, "esac", 4) == 0 &&
                               case_depth > 0) {
                        case_depth--;
                    } else if (case_depth == 0) {
                        if ((wlen == 5 &&
                             strncmp(source + i, "while", 5) == 0) ||
                            (wlen == 5 &&
                             strncmp(source + i, "until", 5) == 0) ||
                            (wlen == 3 && strncmp(source + i, "for", 3) == 0)) {
                            loop_depth++;
                        } else if (wlen == 2 &&
                                   strncmp(source + i, "do", 2) == 0 &&
                                   loop_depth == 0) {
                            words_end = i;
                            body_start = j;
                            break;
                        } else if (wlen == 4 &&
                                   strncmp(source + i, "done", 4) == 0 &&
                                   loop_depth > 0) {
                            loop_depth--;
                        }
                    }
                }
                i = j - 1;
                continue;
            }
            if (paren_depth == 0 && brace_depth == 0 && case_depth == 0 &&
                loop_depth == 0 && (ch == ';' || ch == '\n')) {
                size_t j;

                j = i + 1;
                while (isspace((unsigned char)source[j])) {
                    j++;
                }
                if (strncmp(source + j, "do", 2) == 0 &&
                    keyword_boundary(source[j + 2])) {
                    words_end = i;
                    body_start = j + 2;
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

    if (words_end == 0 && !implicit_words) {
        return false;
    }

have_body_start:
    while (isspace((unsigned char)source[body_start])) {
        body_start++;
    }

    quote = '\0';
    paren_depth = 0;
    brace_depth = 0;
    case_depth = 0;
    loop_depth = 1;
    body_end = 0;

    for (i = body_start; source[i] != '\0'; i++) {
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
            if (paren_depth == 0 && brace_depth == 0 &&
                (isalpha((unsigned char)ch) || ch == '_') &&
                word_starts_command_position(source, i)) {
                size_t j;
                size_t wlen;

                j = i;
                while (isalnum((unsigned char)source[j]) || source[j] == '_') {
                    j++;
                }
                if (keyword_boundary(source[j])) {
                    wlen = j - i;
                    if (wlen == 4 && strncmp(source + i, "case", 4) == 0) {
                        case_depth++;
                    } else if (wlen == 4 &&
                               strncmp(source + i, "esac", 4) == 0 &&
                               case_depth > 0) {
                        case_depth--;
                    } else if (case_depth == 0) {
                        if ((wlen == 5 &&
                             strncmp(source + i, "while", 5) == 0) ||
                            (wlen == 5 &&
                             strncmp(source + i, "until", 5) == 0) ||
                            (wlen == 3 && strncmp(source + i, "for", 3) == 0)) {
                            loop_depth++;
                        } else if (wlen == 4 &&
                                   strncmp(source + i, "done", 4) == 0) {
                            loop_depth--;
                            if (loop_depth == 0) {
                                size_t tail;

                                body_end = i;
                                tail = j;
                                while (isspace((unsigned char)source[tail])) {
                                    tail++;
                                }
                                if (!looks_like_redirection_suffix(source, tail)) {
                                    return false;
                                }

                                *name_out =
                                    dup_trimmed_slice(source, name_start, name_end);
                                *words_out =
                                    dup_trimmed_slice(source, words_start, words_end);
                                *body_out =
                                    dup_trimmed_slice(source, body_start, body_end);
                                if (implicit_words_out != NULL) {
                                    *implicit_words_out = implicit_words;
                                }
                                if (redir_out != NULL) {
                                    *redir_out =
                                        dup_trimmed_slice(source, tail, strlen(source));
                                }
                                return true;
                            }
                        }
                    }
                }
                i = j - 1;
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

    return false;
}

bool compound_needs_single_atom(const char *source) {
    char *trimmed;
    char *cond;
    char *body;
    char *name;
    char *words;
    bool is_until;
    bool matched;

    trimmed = dup_trimmed_slice(source, 0, strlen(source));
    cond = NULL;
    body = NULL;
    name = NULL;
    words = NULL;
    is_until = false;
    matched = false;

    if (parse_simple_while(trimmed, &cond, &body, &is_until, NULL)) {
        matched = true;
    } else if (parse_simple_for(trimmed, &name, &words, &body, NULL, NULL)) {
        matched = true;
    }

    arena_maybe_free(cond);
    arena_maybe_free(body);
    arena_maybe_free(name);
    arena_maybe_free(words);
    arena_maybe_free(trimmed);
    return matched;
}
