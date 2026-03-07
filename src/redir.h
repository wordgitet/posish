/* SPDX-License-Identifier: 0BSD */

/* posish - redirection interface */

#ifndef POSISH_REDIR_H
#define POSISH_REDIR_H

#include <stdbool.h>
#include <stddef.h>

enum redir_kind {
    REDIR_OPEN_READ,
    REDIR_HEREDOC,
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
    bool force_clobber;
    bool strip_tabs;
    bool expand_body;
    char *delimiter;
    char *body_raw;
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
void redir_vec_clone(struct redir_vec *dst, const struct redir_vec *src);

int fd_backup_save(struct fd_backup_vec *backups, int fd);
void fd_backup_restore(struct fd_backup_vec *backups);

int parse_dup_operand(const char *text, struct redir_spec *spec);
int parse_redir_token(const char *token, struct redir_spec *spec, bool *needs_word);

struct shell_state;

int apply_one_redirection(struct shell_state *state,
                          const struct redir_spec *redir, bool noclobber,
                          bool isolate_heredoc_side_effects);
int apply_redirections(struct shell_state *state, const struct redir_vec *redirs,
                       bool save_restore, bool noclobber,
                       bool isolate_heredoc_side_effects,
                       struct fd_backup_vec *backups);
int redir_expand_operands(struct shell_state *state, struct redir_vec *redirs,
                          bool *saw_cmdsub_out,
                          int *last_cmdsub_status_out);
int prepare_runtime_redirections(struct shell_state *state,
                                 const struct redir_vec *src,
                                 struct redir_vec *dst);

#endif
