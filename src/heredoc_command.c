/* SPDX-License-Identifier: 0BSD */

/* posish - here-document processing */

#include "heredoc_command.h"

#include "arena.h"
#include "error.h"
#include "expand.h"
#include "lexer.h"
#include "redir.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
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

static void heredoc_specs_free(struct heredoc_spec *specs, size_t count,
                               bool unlink_files) {
    size_t i;

    for (i = 0; i < count; i++) {
        free(specs[i].delimiter);
        free(specs[i].placeholder);
        free(specs[i].body);
        if (unlink_files && specs[i].temp_path != NULL) {
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

static int append_slice(char **buf, size_t *len, size_t *cap, const char *src,
                        size_t start, size_t end);

static bool heredoc_fd_boundary(char ch) {
    return isspace((unsigned char)ch) || strchr(";&|(){}<>", ch) != NULL;
}

static bool heredoc_word_boundary(char ch) {
    return ch == '\0' || isspace((unsigned char)ch) ||
           strchr(";&|()<>", ch) != NULL;
}

static int append_text(char **buf, size_t *len, size_t *cap, const char *text) {
    return append_slice(buf, len, cap, text, 0, strlen(text));
}

static bool command_is_redirection_only(const char *command) {
    struct token_vec tokens;
    size_t i;
    bool has_command_word;

    has_command_word = false;
    if (lexer_split_words(command, &tokens) != 0) {
        return false;
    }

    for (i = 0; i < tokens.len; i++) {
        struct redir_spec spec;
        bool needs_word;

        if (parse_redir_token(tokens.items[i], &spec, &needs_word)) {
            free(spec.path);
            if (needs_word && i + 1 < tokens.len) {
                i++;
            }
            continue;
        }

        has_command_word = true;
        break;
    }

    lexer_free_tokens(&tokens);
    return !has_command_word;
}

static int parse_command_heredocs(const char *command, char **base_command_out,
                                  struct heredoc_spec **specs_out,
                                  size_t *spec_count_out) {
    char *base_command;
    size_t base_len;
    size_t base_cap;
    struct heredoc_spec *specs;
    size_t spec_count;
    size_t i;

    *base_command_out = NULL;
    *specs_out = NULL;
    *spec_count_out = 0;

    base_command = NULL;
    base_len = 0;
    base_cap = 0;
    specs = NULL;
    spec_count = 0;
    i = 0;
    while (command[i] != '\0') {
        size_t copy_start;
        char quote;

        copy_start = i;
        quote = '\0';

        while (command[i] != '\0') {
            char ch;

            ch = command[i];
            if (quote == '\0') {
                if (ch == '\\' && command[i + 1] != '\0') {
                    i += 2;
                    continue;
                }
                if (ch == '\'' || ch == '"') {
                    quote = ch;
                    i++;
                    continue;
                }
                if (ch == '<' && command[i + 1] == '<') {
                    size_t fd_start;
                    bool explicit_fd;
                    int target_fd;
                    size_t word_start;
                    size_t word_end;
                    bool strip_tabs;
                    char delim_quote;
                    char *raw_delim;
                    struct heredoc_spec spec;
                    char placeholder_name[64];

                    fd_start = i;
                    while (fd_start > 0 &&
                           isdigit((unsigned char)command[fd_start - 1])) {
                        fd_start--;
                    }
                    explicit_fd = fd_start < i &&
                                  (fd_start == 0 ||
                                   heredoc_fd_boundary(command[fd_start - 1]));
                    target_fd = STDIN_FILENO;
                    if (explicit_fd) {
                        size_t dpos;

                        for (dpos = fd_start; dpos < i; dpos++) {
                            target_fd = target_fd * 10 + (command[dpos] - '0');
                        }
                    }

                    append_slice(&base_command, &base_len, &base_cap, command,
                                 copy_start, i);

                    i += 2;
                    strip_tabs = false;
                    if (command[i] == '-') {
                        strip_tabs = true;
                        i++;
                    }
                    while (command[i] == ' ' || command[i] == '\t') {
                        i++;
                    }
                    if (command[i] == '\0' || command[i] == '\n') {
                        posish_errorf("missing heredoc delimiter");
                        free(base_command);
                        heredoc_specs_free(specs, spec_count, true);
                        return -1;
                    }

                    word_start = i;
                    delim_quote = '\0';
                    while (command[i] != '\0') {
                        ch = command[i];
                        if (delim_quote == '\0') {
                            if (ch == '\\' && command[i + 1] != '\0') {
                                i += 2;
                                continue;
                            }
                            if (ch == '\'' || ch == '"') {
                                delim_quote = ch;
                                i++;
                                continue;
                            }
                            if (heredoc_word_boundary(ch)) {
                                break;
                            }
                            i++;
                            continue;
                        }

                        if (delim_quote == '\'' && ch == '\'') {
                            delim_quote = '\0';
                            i++;
                            continue;
                        }
                        if (delim_quote == '"') {
                            if (ch == '\\' && command[i + 1] != '\0') {
                                i += 2;
                                continue;
                            }
                            if (ch == '"') {
                                delim_quote = '\0';
                            }
                        }
                        i++;
                    }
                    if (delim_quote != '\0') {
                        posish_errorf("unterminated heredoc delimiter quote");
                        free(base_command);
                        heredoc_specs_free(specs, spec_count, true);
                        return -1;
                    }

                    word_end = i;
                    if (word_end == word_start) {
                        posish_errorf("missing heredoc delimiter");
                        free(base_command);
                        heredoc_specs_free(specs, spec_count, true);
                        return -1;
                    }

                    raw_delim = arena_xmalloc((word_end - word_start) + 1);
                    memcpy(raw_delim, command + word_start, word_end - word_start);
                    raw_delim[word_end - word_start] = '\0';

                    spec.target_fd = target_fd;
                    spec.strip_tabs = strip_tabs;
                    spec.expand_body = strpbrk(raw_delim, "'\"\\") == NULL;
                    spec.body = NULL;
                    spec.temp_path = NULL;
                    snprintf(placeholder_name, sizeof(placeholder_name),
                             "__POSISH_HEREDOC_%zu__", spec_count);
                    spec.placeholder = arena_xstrdup(placeholder_name);
                    if (quote_remove_word(raw_delim, &spec.delimiter) != 0) {
                        free(raw_delim);
                        free(base_command);
                        heredoc_specs_free(specs, spec_count, true);
                        return -1;
                    }
                    free(raw_delim);

                    append_text(&base_command, &base_len, &base_cap, "<");
                    append_text(&base_command, &base_len, &base_cap,
                                spec.placeholder);
                    if (command[i] != '\0' &&
                        !isspace((unsigned char)command[i])) {
                        append_text(&base_command, &base_len, &base_cap, " ");
                    }
                    heredoc_specs_push(&specs, &spec_count, &spec);
                    copy_start = i;
                    continue;
                }
            } else if (quote == '\'' && ch == '\'') {
                quote = '\0';
            } else if (quote == '"') {
                if (ch == '\\' && command[i + 1] != '\0') {
                    i += 2;
                    continue;
                }
                if (ch == '"') {
                    quote = '\0';
                }
            }
            i++;
        }

        append_slice(&base_command, &base_len, &base_cap, command, copy_start, i);
        if (command[i] == '\0') {
            break;
        }
    }

    if (base_command == NULL) {
        base_command = arena_xstrdup("");
    }

    *base_command_out = base_command;
    *specs_out = specs;
    *spec_count_out = spec_count;
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

static int expand_heredoc_text_isolated(const char *input,
                                        struct shell_state *state,
                                        char **out) {
    int pipefd[2];
    pid_t pid;
    char *buf;
    size_t len;
    size_t cap;
    int status;

    if (pipe(pipefd) != 0) {
        perror("pipe");
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        perror("fork");
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        struct shell_state local_state;
        char *expanded;
        size_t expanded_len;
        size_t written;

        close(pipefd[0]);
        local_state = *state;
        if (expand_heredoc_text(input, &local_state, &expanded) != 0) {
            _exit(1);
        }

        expanded_len = strlen(expanded);
        written = 0;
        while (written < expanded_len) {
            ssize_t nwritten;

            nwritten =
                write(pipefd[1], expanded + written, expanded_len - written);
            if (nwritten < 0) {
                if (errno == EINTR) {
                    continue;
                }
                free(expanded);
                _exit(1);
            }
            written += (size_t)nwritten;
        }
        free(expanded);
        close(pipefd[1]);
        _exit(0);
    }

    close(pipefd[1]);
    buf = arena_xmalloc(64);
    len = 0;
    cap = 64;
    for (;;) {
        ssize_t nread;

        if (len + 64 > cap) {
            cap *= 2;
            buf = xrealloc(buf, cap);
        }

        nread = read(pipefd[0], buf + len, cap - len - 1);
        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("read");
            close(pipefd[0]);
            free(buf);
            return -1;
        }
        if (nread == 0) {
            break;
        }
        len += (size_t)nread;
    }
    close(pipefd[0]);
    buf[len] = '\0';

    for (;;) {
        if (waitpid(pid, &status, 0) < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("waitpid");
            free(buf);
            return -1;
        }
        break;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        free(buf);
        return -1;
    }

    *out = buf;
    return 0;
}

static int consume_heredoc_bodies(struct shell_state *state, const char *source,
                                  size_t start_pos,
                                  bool isolate_expansion_side_effects,
                                  struct heredoc_spec *specs, size_t spec_count,
                                  size_t *new_pos_out, bool *incomplete_out) {
    size_t pos;
    size_t sidx;

    *incomplete_out = false;
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
                *incomplete_out = true;
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
                *incomplete_out = true;
                free(body);
                return -1;
            }
        }

        if (body == NULL) {
            body = arena_xstrdup("");
        }
        if (specs[sidx].expand_body) {
            char *expanded;

            if ((isolate_expansion_side_effects &&
                 expand_heredoc_text_isolated(body, state, &expanded) != 0) ||
                (!isolate_expansion_side_effects &&
                 expand_heredoc_text(body, state, &expanded) != 0)) {
                free(body);
                return -1;
            }
            free(body);
            body = expanded;
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
                                  heredoc_command_runner_fn runner,
                                  bool preserve_tempfiles) {
    char *base_command;
    struct heredoc_spec *specs;
    size_t spec_count;
    char *rewritten;
    int status;
    bool isolate_expansion_side_effects;
    bool incomplete;

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
    isolate_expansion_side_effects = command_is_redirection_only(base_command);

    if (consume_heredoc_bodies(state, source, body_pos,
                               isolate_expansion_side_effects, specs, spec_count,
                               new_pos_out, &incomplete) != 0) {
        if (incomplete) {
            *handled = false;
            heredoc_specs_free(specs, spec_count, true);
            free(base_command);
            return 0;
        }
        heredoc_specs_free(specs, spec_count, true);
        free(base_command);
        return -1;
    }
    if (write_heredoc_tempfiles(specs, spec_count) != 0) {
        heredoc_specs_free(specs, spec_count, true);
        free(base_command);
        return -1;
    }

    rewritten = build_heredoc_command(base_command, specs, spec_count);
    status = runner(state, rewritten);
    *status_out = status;

    heredoc_specs_free(specs, spec_count, !preserve_tempfiles);
    free(base_command);
    free(rewritten);
    return 0;
}
