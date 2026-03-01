/* SPDX-License-Identifier: 0BSD */

/* posish - here-document processing */

#include "heredoc_command.h"

#include "arena.h"
#include "error.h"
#include "lexer.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct heredoc_spec {
    int target_fd;
    bool strip_tabs;
    bool expand_body;
    char *delimiter;
    char *placeholder;
    char *body;
    char *temp_path;
};

static void *xrealloc(void *ptr, size_t size) {
    void *p;

    p = realloc(ptr, size);
    if (p == NULL) {
        perror("realloc");
        exit(EXIT_FAILURE);
    }
    return p;
}

static void free_string_vec(char **vec, size_t len) {
    size_t i;

    for (i = 0; i < len; i++) {
        free(vec[i]);
    }
    free(vec);
}

static void append_char(char **buf, size_t *len, size_t *cap, char ch) {
    if (*len + 2 > *cap) {
        size_t new_cap;

        new_cap = *cap == 0 ? 32 : *cap;
        while (*len + 2 > new_cap) {
            new_cap *= 2;
        }
        *buf = xrealloc(*buf, new_cap);
        *cap = new_cap;
    }

    (*buf)[(*len)++] = ch;
    (*buf)[*len] = '\0';
}

static void heredoc_specs_free(struct heredoc_spec *specs, size_t count) {
    size_t i;

    for (i = 0; i < count; i++) {
        free(specs[i].delimiter);
        free(specs[i].placeholder);
        free(specs[i].body);
        if (specs[i].temp_path != NULL) {
            unlink(specs[i].temp_path);
        }
        free(specs[i].temp_path);
    }
    free(specs);
}

static int heredoc_specs_push(struct heredoc_spec **specs, size_t *count,
                              const struct heredoc_spec *spec) {
    *specs = xrealloc(*specs, sizeof(**specs) * (*count + 1));
    (*specs)[*count] = *spec;
    (*count)++;
    return 0;
}

static int quote_remove_word(const char *word, char **out_word) {
    size_t i;
    char quote;
    char *buf;
    size_t len;
    size_t cap;

    i = 0;
    quote = '\0';
    buf = NULL;
    len = 0;
    cap = 0;

    while (word[i] != '\0') {
        char ch;

        ch = word[i];
        if (quote == '\0') {
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
            if (ch == '\\' && word[i + 1] != '\0') {
                i++;
                ch = word[i];
            }
            append_char(&buf, &len, &cap, ch);
            i++;
            continue;
        }

        if (quote == '\'' && ch == '\'') {
            quote = '\0';
            i++;
            continue;
        }
        if (quote == '"' && ch == '"') {
            quote = '\0';
            i++;
            continue;
        }
        if (quote == '"' && ch == '\\' && word[i + 1] != '\0' &&
            (word[i + 1] == '$' || word[i + 1] == '`' || word[i + 1] == '"' ||
             word[i + 1] == '\\' || word[i + 1] == '\n')) {
            i++;
            ch = word[i];
        }

        append_char(&buf, &len, &cap, ch);
        i++;
    }

    if (quote != '\0') {
        posish_errorf("unterminated heredoc delimiter quote");
        free(buf);
        return -1;
    }

    if (buf == NULL) {
        buf = arena_xstrdup("");
    }
    *out_word = buf;
    return 0;
}

static bool parse_heredoc_token(const char *token, int *target_fd,
                                bool *strip_tabs, const char **inline_delim,
                                bool *needs_word) {
    size_t pos;
    int fd;
    bool have_fd;

    pos = 0;
    fd = 0;
    have_fd = false;
    while (isdigit((unsigned char)token[pos])) {
        have_fd = true;
        fd = fd * 10 + (token[pos] - '0');
        pos++;
    }

    if (token[pos] != '<' || token[pos + 1] != '<') {
        return false;
    }
    pos += 2;

    *strip_tabs = false;
    if (token[pos] == '-') {
        *strip_tabs = true;
        pos++;
    }

    *target_fd = have_fd ? fd : STDIN_FILENO;
    if (token[pos] == '\0') {
        *needs_word = true;
        *inline_delim = NULL;
    } else {
        *needs_word = false;
        *inline_delim = token + pos;
    }
    return true;
}

static int push_token_copy(char ***tokens, size_t *count, const char *token) {
    *tokens = xrealloc(*tokens, sizeof(**tokens) * (*count + 1));
    (*tokens)[*count] = arena_xstrdup(token);
    (*count)++;
    return 0;
}

static char *join_tokens(char *const tokens[], size_t count) {
    size_t i;
    size_t len;
    char *out;
    size_t pos;

    len = 1;
    for (i = 0; i < count; i++) {
        len += strlen(tokens[i]) + 1;
    }

    out = arena_xmalloc(len);
    pos = 0;
    out[0] = '\0';
    for (i = 0; i < count; i++) {
        size_t n;
        if (i > 0) {
            out[pos++] = ' ';
        }
        n = strlen(tokens[i]);
        memcpy(out + pos, tokens[i], n);
        pos += n;
    }
    out[pos] = '\0';
    return out;
}

static int parse_command_heredocs(const char *command, char **base_command_out,
                                  struct heredoc_spec **specs_out,
                                  size_t *spec_count_out) {
    struct token_vec tokens;
    char **kept_tokens;
    size_t kept_count;
    struct heredoc_spec *specs;
    size_t spec_count;
    size_t i;

    *base_command_out = NULL;
    *specs_out = NULL;
    *spec_count_out = 0;

    if (lexer_split_words(command, &tokens) != 0) {
        return -1;
    }

    kept_tokens = NULL;
    kept_count = 0;
    specs = NULL;
    spec_count = 0;

    for (i = 0; i < tokens.len; i++) {
        int target_fd;
        bool strip_tabs;
        const char *inline_delim;
        bool needs_word;
        const char *raw_delim;
        struct heredoc_spec spec;
        char placeholder_name[64];
        char redir_token[96];

        if (!parse_heredoc_token(tokens.items[i], &target_fd, &strip_tabs,
                                 &inline_delim, &needs_word)) {
            push_token_copy(&kept_tokens, &kept_count, tokens.items[i]);
            continue;
        }

        if (needs_word) {
            i++;
            if (i >= tokens.len) {
                posish_errorf("missing heredoc delimiter");
                free_string_vec(kept_tokens, kept_count);
                heredoc_specs_free(specs, spec_count);
                lexer_free_tokens(&tokens);
                return -1;
            }
            raw_delim = tokens.items[i];
        } else {
            raw_delim = inline_delim;
        }

        spec.target_fd = target_fd;
        spec.strip_tabs = strip_tabs;
        spec.expand_body = strpbrk(raw_delim, "'\"\\") == NULL;
        spec.body = NULL;
        spec.temp_path = NULL;
        snprintf(placeholder_name, sizeof(placeholder_name),
                 "__POSISH_HEREDOC_%zu__", spec_count);
        spec.placeholder = arena_xstrdup(placeholder_name);
        if (quote_remove_word(raw_delim, &spec.delimiter) != 0) {
            free_string_vec(kept_tokens, kept_count);
            heredoc_specs_free(specs, spec_count);
            lexer_free_tokens(&tokens);
            return -1;
        }

        if (spec.target_fd == STDIN_FILENO) {
            snprintf(redir_token, sizeof(redir_token), "<%s", spec.placeholder);
        } else {
            snprintf(redir_token, sizeof(redir_token), "%d<%s", spec.target_fd,
                     spec.placeholder);
        }
        push_token_copy(&kept_tokens, &kept_count, redir_token);
        heredoc_specs_push(&specs, &spec_count, &spec);
    }

    *base_command_out = join_tokens(kept_tokens, kept_count);
    *specs_out = specs;
    *spec_count_out = spec_count;

    free_string_vec(kept_tokens, kept_count);
    lexer_free_tokens(&tokens);
    return 0;
}

static int append_slice(char **buf, size_t *len, size_t *cap, const char *src,
                        size_t start, size_t end) {
    size_t n;

    n = end - start;
    if (*len + n + 1 > *cap) {
        size_t new_cap;

        new_cap = *cap == 0 ? 64 : *cap;
        while (*len + n + 1 > new_cap) {
            new_cap *= 2;
        }
        *buf = xrealloc(*buf, new_cap);
        *cap = new_cap;
    }

    memcpy(*buf + *len, src + start, n);
    *len += n;
    (*buf)[*len] = '\0';
    return 0;
}

static int consume_heredoc_bodies(const char *source, size_t start_pos,
                                  struct heredoc_spec *specs, size_t spec_count,
                                  size_t *new_pos_out) {
    size_t pos;
    size_t sidx;

    pos = start_pos;

    for (sidx = 0; sidx < spec_count; sidx++) {
        char *body;
        size_t body_len;
        size_t body_cap;

        body = NULL;
        body_len = 0;
        body_cap = 0;

        for (;;) {
            size_t line_start;
            size_t line_end;
            size_t cmp_start;
            size_t delim_len;

            if (source[pos] == '\0') {
                posish_errorf("unexpected EOF while looking for heredoc delimiter");
                free(body);
                return -1;
            }

            line_start = pos;
            while (source[pos] != '\0' && source[pos] != '\n') {
                pos++;
            }
            line_end = pos;

            cmp_start = line_start;
            if (specs[sidx].strip_tabs) {
                while (cmp_start < line_end && source[cmp_start] == '\t') {
                    cmp_start++;
                }
            }

            delim_len = strlen(specs[sidx].delimiter);
            if (line_end - cmp_start == delim_len &&
                memcmp(source + cmp_start, specs[sidx].delimiter, delim_len) == 0) {
                if (source[pos] == '\n') {
                    pos++;
                }
                break;
            }

            if (specs[sidx].strip_tabs) {
                while (line_start < line_end && source[line_start] == '\t') {
                    line_start++;
                }
            }
            append_slice(&body, &body_len, &body_cap, source, line_start, line_end);

            if (source[pos] == '\n') {
                append_slice(&body, &body_len, &body_cap, "\n", 0, 1);
                pos++;
            } else {
                posish_errorf("unexpected EOF while reading heredoc body");
                free(body);
                return -1;
            }
        }

        if (body == NULL) {
            body = arena_xstrdup("");
        }
        specs[sidx].body = body;
    }

    *new_pos_out = pos;
    return 0;
}

static int write_heredoc_tempfiles(struct heredoc_spec *specs, size_t spec_count) {
    size_t i;

    for (i = 0; i < spec_count; i++) {
        char template_path[] = "/tmp/posish-heredoc-XXXXXX";
        int fd;
        size_t body_len;

        fd = mkstemp(template_path);
        if (fd < 0) {
            perror("mkstemp");
            return -1;
        }

        body_len = strlen(specs[i].body);
        if (body_len > 0) {
            ssize_t nwritten;

            nwritten = write(fd, specs[i].body, body_len);
            if (nwritten < 0 || (size_t)nwritten != body_len) {
                perror("write");
                close(fd);
                unlink(template_path);
                return -1;
            }
        }
        close(fd);
        specs[i].temp_path = arena_xstrdup(template_path);
    }

    return 0;
}

static char *replace_all(const char *in, const char *needle,
                         const char *replacement) {
    const char *p;
    size_t in_len;
    size_t needle_len;
    size_t repl_len;
    size_t count;
    char *out;
    size_t out_len;
    char *dst;

    in_len = strlen(in);
    needle_len = strlen(needle);
    repl_len = strlen(replacement);
    if (needle_len == 0) {
        return arena_xstrdup(in);
    }

    count = 0;
    p = in;
    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += needle_len;
    }
    if (count == 0) {
        return arena_xstrdup(in);
    }

    out_len = in_len + count * (repl_len - needle_len);
    out = arena_xmalloc(out_len + 1);
    dst = out;
    p = in;
    while (*p != '\0') {
        const char *hit;
        size_t chunk;

        hit = strstr(p, needle);
        if (hit == NULL) {
            strcpy(dst, p);
            break;
        }
        chunk = (size_t)(hit - p);
        memcpy(dst, p, chunk);
        dst += chunk;
        memcpy(dst, replacement, repl_len);
        dst += repl_len;
        p = hit + needle_len;
    }
    out[out_len] = '\0';
    return out;
}

static char *build_heredoc_command(const char *base_command,
                                   const struct heredoc_spec *specs,
                                   size_t spec_count) {
    size_t i;
    char *out;

    out = arena_xstrdup(base_command);
    for (i = 0; i < spec_count; i++) {
        char *replaced;

        replaced = replace_all(out, specs[i].placeholder, specs[i].temp_path);
        free(out);
        out = replaced;
    }
    return out;
}

int maybe_execute_heredoc_command(struct shell_state *state, const char *command,
                                  const char *source, size_t body_pos,
                                  size_t *new_pos_out, bool *handled,
                                  int *status_out,
                                  heredoc_command_runner_fn runner) {
    char *base_command;
    struct heredoc_spec *specs;
    size_t spec_count;
    char *rewritten;
    int status;

    *handled = false;
    *status_out = 0;
    *new_pos_out = body_pos;

    if (parse_command_heredocs(command, &base_command, &specs, &spec_count) != 0) {
        return -1;
    }
    if (spec_count == 0) {
        free(base_command);
        return 0;
    }

    *handled = true;
    if (consume_heredoc_bodies(source, body_pos, specs, spec_count, new_pos_out) != 0) {
        heredoc_specs_free(specs, spec_count);
        free(base_command);
        return -1;
    }
    if (write_heredoc_tempfiles(specs, spec_count) != 0) {
        heredoc_specs_free(specs, spec_count);
        free(base_command);
        return -1;
    }

    rewritten = build_heredoc_command(base_command, specs, spec_count);
    status = runner(state, rewritten);
    *status_out = status;

    heredoc_specs_free(specs, spec_count);
    free(base_command);
    free(rewritten);
    return 0;
}
