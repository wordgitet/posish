/* SPDX-License-Identifier: 0BSD */

/* posish - redirection interface */

#ifndef POSISH_REDIR_H
#define POSISH_REDIR_H

#include <stdbool.h>
#include <stddef.h>

enum redir_kind {
    REDIR_OPEN_READ,
    REDIR_OPEN_RDWR,
    REDIR_OPEN_WRITE,
    REDIR_OPEN_APPEND,
    REDIR_DUP_IN,
    REDIR_DUP_OUT,
    REDIR_CLOSE,
};

struct redir_spec {
    enum redir_kind kind;
    int target_fd;
    int source_fd;
    char *path;
};

struct redir_vec {
    struct redir_spec *items;
    size_t len;
};

struct fd_backup {
    int fd;
    int saved_fd;
    bool was_open;
};

struct fd_backup_vec {
    struct fd_backup *items;
    size_t len;
};

void redir_vec_free(struct redir_vec *redirs);
int redir_vec_push(struct redir_vec *redirs, const struct redir_spec *spec);

int fd_backup_save(struct fd_backup_vec *backups, int fd);
void fd_backup_restore(struct fd_backup_vec *backups);

int parse_dup_operand(const char *text, struct redir_spec *spec);
int parse_redir_token(const char *token, struct redir_spec *spec, bool *needs_word);

int apply_one_redirection(const struct redir_spec *redir);
int apply_redirections(const struct redir_vec *redirs, bool save_restore,
                       struct fd_backup_vec *backups);

#endif
