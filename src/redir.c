/* SPDX-License-Identifier: 0BSD */

/* posish - redirection logic */

#include "redir.h"

#include "arena.h"
#include "error.h"
#include "expand.h"
#include "lexer.h"
#include "shell.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

void redir_vec_free(struct redir_vec *redirs) {
    /*
     * Redirection vectors are arena-backed and reclaimed by the surrounding
     * command/script arena reset.
     */
    redirs->items = NULL;
    redirs->len = 0;
}

int redir_vec_push(struct redir_vec *redirs, const struct redir_spec *spec) {
    redirs->items = arena_xrealloc(redirs->items,
                                   sizeof(*redirs->items) * (redirs->len + 1));
    redirs->items[redirs->len++] = *spec;
    return 0;
}

void redir_vec_clone(struct redir_vec *dst, const struct redir_vec *src) {
    size_t i;

    dst->items = NULL;
    dst->len = 0;
    if (src == NULL || src->len == 0) {
        return;
    }

    dst->items = arena_xrealloc(dst->items, sizeof(*dst->items) * src->len);
    dst->len = src->len;
    for (i = 0; i < src->len; i++) {
        dst->items[i] = src->items[i];
        if (src->items[i].path != NULL) {
            dst->items[i].path = arena_xstrdup(src->items[i].path);
        }
        if (src->items[i].delimiter != NULL) {
            dst->items[i].delimiter = arena_xstrdup(src->items[i].delimiter);
        }
        if (src->items[i].body_raw != NULL) {
            dst->items[i].body_raw = arena_xstrdup(src->items[i].body_raw);
        }
    }
}

int fd_backup_save(struct fd_backup_vec *backups, int fd) {
    size_t i;
    struct fd_backup b;

    for (i = 0; i < backups->len; i++) {
        if (backups->items[i].fd == fd) {
            return 0;
        }
    }

    b.fd = fd;
    b.saved_fd = fcntl(fd, F_DUPFD, 10);
    if (b.saved_fd < 0) {
        if (errno != EBADF) {
            perror("dup");
            return -1;
        }
        b.was_open = false;
    } else {
        b.was_open = true;
    }

    backups->items =
        arena_xrealloc(backups->items,
                       sizeof(*backups->items) * (backups->len + 1));
    backups->items[backups->len++] = b;
    return 0;
}

void fd_backup_restore(struct fd_backup_vec *backups) {
    size_t i;

    for (i = backups->len; i > 0; i--) {
        struct fd_backup *b;

        b = &backups->items[i - 1];
        if (b->was_open) {
            if (dup2(b->saved_fd, b->fd) < 0) {
                perror("dup2");
            }
            close(b->saved_fd);
        } else {
            close(b->fd);
        }
    }

    /*
     * Backup vector storage is arena-backed and reclaimed by arena reset.
     */
    backups->items = NULL;
    backups->len = 0;
}

static int write_all(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t nwritten;

        nwritten = write(fd, buf, len);
        if (nwritten < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (nwritten == 0) {
            errno = EIO;
            return -1;
        }
        buf += (size_t)nwritten;
        len -= (size_t)nwritten;
    }
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

        close(pipefd[0]);
        local_state = *state;
        arena_init(&local_state.arena_perm, state->arena_perm.default_block_size);
        arena_init(&local_state.arena_script,
                   state->arena_script.default_block_size);
        arena_init(&local_state.arena_cmd, state->arena_cmd.default_block_size);
        arena_set_current(&local_state.arena_perm);
        if (expand_heredoc_text(input, &local_state, &expanded) != 0) {
            _exit(1);
        }
        if (write_all(pipefd[1], expanded, strlen(expanded)) != 0) {
            _exit(1);
        }
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
            buf = arena_xrealloc(buf, cap);
        }

        nread = read(pipefd[0], buf + len, cap - len - 1);
        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("read");
            close(pipefd[0]);
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
            return -1;
        }
        break;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return -1;
    }

    *out = buf;
    return 0;
}

static int create_heredoc_fd(void) {
#ifdef SYS_memfd_create
    int fd;

    fd = (int)syscall(SYS_memfd_create, "posish-heredoc", MFD_CLOEXEC);
    if (fd >= 0) {
        return fd;
    }
    if (errno != ENOSYS) {
        return -1;
    }
#endif
    {
        char template_path[] = "/tmp/posish-heredoc-XXXXXX";
        int fd;

        fd = mkstemp(template_path);
        if (fd < 0) {
            return -1;
        }
        unlink(template_path);
        (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
        return fd;
    }
}

static int materialize_heredoc_fd(struct shell_state *state,
                                  const struct redir_spec *redir,
                                  bool isolate_heredoc_side_effects,
                                  int *fd_out) {
    char *expanded;
    const char *text;
    int fd;

    expanded = NULL;
    text = redir->body_raw != NULL ? redir->body_raw : "";
    if (redir->expand_body) {
        if ((isolate_heredoc_side_effects
                 ? expand_heredoc_text_isolated(text, state, &expanded)
                 : expand_heredoc_text(text, state, &expanded)) != 0) {
            return -1;
        }
        text = expanded;
    }
    fd = create_heredoc_fd();
    if (fd < 0) {
        perror("heredoc fd");
        return -1;
    }
    if (write_all(fd, text, strlen(text)) != 0 || lseek(fd, 0, SEEK_SET) < 0) {
        perror("heredoc");
        close(fd);
        return -1;
    }

    *fd_out = fd;
    return 0;
}

static int parse_fd_text(const char *text, int *fd_out) {
    size_t i;
    int value;

    if (text[0] == '\0') {
        return -1;
    }

    value = 0;
    for (i = 0; text[i] != '\0'; i++) {
        if (!isdigit((unsigned char)text[i])) {
            return -1;
        }
        value = value * 10 + (text[i] - '0');
    }

    *fd_out = value;
    return 0;
}

int parse_dup_operand(const char *text, struct redir_spec *spec) {
    int fd;

    if (strcmp(text, "-") == 0) {
        spec->kind = REDIR_CLOSE;
        spec->source_fd = -1;
        return 0;
    }

    if (parse_fd_text(text, &fd) != 0) {
        return -1;
    }

    spec->source_fd = fd;
    return 0;
}

int parse_redir_token(const char *token, struct redir_spec *spec, bool *needs_word) {
    size_t pos;
    int fd;
    bool have_fd;
    const char *rest;

    spec->kind = REDIR_OPEN_READ;
    spec->target_fd = 0;
    spec->source_fd = -1;
    spec->force_clobber = false;
    spec->strip_tabs = false;
    spec->expand_body = false;
    spec->delimiter = NULL;
    spec->body_raw = NULL;
    spec->path = NULL;
    *needs_word = false;

    pos = 0;
    fd = 0;
    have_fd = false;
    while (isdigit((unsigned char)token[pos])) {
        have_fd = true;
        fd = fd * 10 + (token[pos] - '0');
        pos++;
    }

    if (token[pos] == '\0' || (token[pos] != '<' && token[pos] != '>')) {
        return 0;
    }

    if (token[pos] == '<') {
        if (token[pos + 1] == '<') {
            spec->kind = REDIR_HEREDOC;
            pos += 2;
            if (token[pos] == '-') {
                spec->strip_tabs = true;
                pos += 1;
            }
        } else if (token[pos + 1] == '>') {
            spec->kind = REDIR_OPEN_RDWR;
            pos += 2;
        } else if (token[pos + 1] == '&') {
            spec->kind = REDIR_DUP_IN;
            pos += 2;
        } else {
            spec->kind = REDIR_OPEN_READ;
            pos += 1;
        }
    } else {
        if (token[pos + 1] == '>') {
            spec->kind = REDIR_OPEN_APPEND;
            pos += 2;
        } else if (token[pos + 1] == '|') {
            spec->kind = REDIR_OPEN_WRITE;
            spec->force_clobber = true;
            pos += 2;
        } else if (token[pos + 1] == '&') {
            spec->kind = REDIR_DUP_OUT;
            pos += 2;
        } else {
            spec->kind = REDIR_OPEN_WRITE;
            pos += 1;
        }
    }

    if (have_fd) {
        spec->target_fd = fd;
    } else if (spec->kind == REDIR_OPEN_READ || spec->kind == REDIR_OPEN_RDWR ||
               spec->kind == REDIR_DUP_IN || spec->kind == REDIR_HEREDOC) {
        spec->target_fd = STDIN_FILENO;
    } else {
        spec->target_fd = STDOUT_FILENO;
    }

    rest = token + pos;
    if (spec->kind == REDIR_DUP_IN || spec->kind == REDIR_DUP_OUT) {
        if (*rest == '\0') {
            *needs_word = true;
            return 1;
        }
        spec->path = arena_xstrdup(rest);
        return 1;
    }

    if (*rest == '\0') {
        *needs_word = true;
        return 1;
    }

    spec->path = arena_xstrdup(rest);
    return 1;
}

int apply_one_redirection(struct shell_state *state,
                          const struct redir_spec *redir, bool noclobber,
                          bool isolate_heredoc_side_effects) {
    int opened_fd;

    opened_fd = -1;
    if (redir->kind == REDIR_HEREDOC) {
        if (materialize_heredoc_fd(state, redir, isolate_heredoc_side_effects,
                                   &opened_fd) != 0) {
            return 1;
        }
    } else if (redir->kind == REDIR_OPEN_READ) {
        opened_fd = open(redir->path, O_RDONLY);
    } else if (redir->kind == REDIR_OPEN_RDWR) {
        opened_fd = open(redir->path, O_RDWR | O_CREAT, 0666);
    } else if (redir->kind == REDIR_OPEN_WRITE) {
        if (noclobber && !redir->force_clobber) {
            struct stat st;

            if (stat(redir->path, &st) == 0 && S_ISREG(st.st_mode)) {
                errno = EEXIST;
                opened_fd = -1;
            } else {
                opened_fd = open(redir->path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            }
        } else {
            opened_fd = open(redir->path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        }
    } else if (redir->kind == REDIR_OPEN_APPEND) {
        opened_fd = open(redir->path, O_WRONLY | O_CREAT | O_APPEND, 0666);
    }

    if (opened_fd >= 0) {
        if (opened_fd != redir->target_fd) {
            if (dup2(opened_fd, redir->target_fd) < 0) {
                perror("dup2");
                close(opened_fd);
                return 1;
            }
            close(opened_fd);
        }
        return 0;
    }

    if (redir->kind == REDIR_OPEN_READ || redir->kind == REDIR_OPEN_RDWR ||
        redir->kind == REDIR_OPEN_WRITE || redir->kind == REDIR_OPEN_APPEND) {
        perror(redir->path);
        return 1;
    }

    if (redir->kind == REDIR_HEREDOC) {
        perror("heredoc");
        return 1;
    }

    if (redir->kind == REDIR_DUP_IN || redir->kind == REDIR_DUP_OUT) {
        int flags;

        flags = fcntl(redir->source_fd, F_GETFL);
        if (flags < 0) {
            perror("fcntl");
            return 1;
        }
        if (redir->kind == REDIR_DUP_IN && (flags & O_ACCMODE) == O_WRONLY) {
            errno = EBADF;
            perror("dup2");
            return 1;
        }
        if (redir->kind == REDIR_DUP_OUT && (flags & O_ACCMODE) == O_RDONLY) {
            errno = EBADF;
            perror("dup2");
            return 1;
        }
        if (dup2(redir->source_fd, redir->target_fd) < 0) {
            perror("dup2");
            return 1;
        }
        return 0;
    }

    if (close(redir->target_fd) != 0 && errno != EBADF) {
        perror("close");
        return 1;
    }
    return 0;
}

int apply_redirections(struct shell_state *state, const struct redir_vec *redirs,
                       bool save_restore, bool noclobber,
                       bool isolate_heredoc_side_effects,
                       struct fd_backup_vec *backups) {
    size_t i;

    for (i = 0; i < redirs->len; i++) {
        if (save_restore) {
            if (fd_backup_save(backups, redirs->items[i].target_fd) != 0) {
                return 1;
            }
        }

        if (apply_one_redirection(state, &redirs->items[i], noclobber,
                                  isolate_heredoc_side_effects) != 0) {
            return 1;
        }
    }

    return 0;
}

int redir_expand_operands(struct shell_state *state, struct redir_vec *redirs,
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

int prepare_runtime_redirections(struct shell_state *state,
                                 const struct redir_vec *src,
                                 struct redir_vec *dst) {
    dst->items = NULL;
    dst->len = 0;
    redir_vec_clone(dst, src);
    if (redir_expand_operands(state, dst, NULL, NULL) != 0) {
        redir_vec_free(dst);
        return 1;
    }
    return 0;
}
