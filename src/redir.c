/* SPDX-License-Identifier: 0BSD */

/* posish - redirection logic */

#include "redir.h"

#include "arena.h"
#include "error.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void redir_vec_free(struct redir_vec *redirs) {
    size_t i;

    for (i = 0; i < redirs->len; i++) {
        arena_maybe_free(redirs->items[i].path);
    }
    arena_maybe_free(redirs->items);
    redirs->items = NULL;
    redirs->len = 0;
}

int redir_vec_push(struct redir_vec *redirs, const struct redir_spec *spec) {
    redirs->items = arena_xrealloc(redirs->items,
                                   sizeof(*redirs->items) * (redirs->len + 1));
    redirs->items[redirs->len++] = *spec;
    return 0;
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

    arena_maybe_free(backups->items);
    backups->items = NULL;
    backups->len = 0;
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
        if (token[pos + 1] == '>') {
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
               spec->kind == REDIR_DUP_IN) {
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

int apply_one_redirection(const struct redir_spec *redir, bool noclobber) {
    int opened_fd;

    opened_fd = -1;
    if (redir->kind == REDIR_OPEN_READ) {
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

int apply_redirections(const struct redir_vec *redirs, bool save_restore,
                       bool noclobber,
                       struct fd_backup_vec *backups) {
    size_t i;

    for (i = 0; i < redirs->len; i++) {
        if (save_restore) {
            if (fd_backup_save(backups, redirs->items[i].target_fd) != 0) {
                return 1;
            }
        }

        if (apply_one_redirection(&redirs->items[i], noclobber) != 0) {
            return 1;
        }
    }

    return 0;
}
