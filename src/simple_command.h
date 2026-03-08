/* SPDX-License-Identifier: 0BSD */

/* posish - simple command execution helpers */

#ifndef POSISH_SIMPLE_COMMAND_H
#define POSISH_SIMPLE_COMMAND_H

#include "ast.h"
#include "lexer.h"
#include "redir.h"
#include "shell.h"

#include <stdbool.h>

typedef int (*simple_command_body_runner)(struct shell_state *state,
                                          const char *source);

void simple_command_word_vec_free(struct ast_word_vec *words);
int simple_command_collect_words_and_redirs(const struct token_vec *expanded,
                                            struct ast_word_vec *words,
                                            struct redir_vec *redirs);
int simple_command_parse_redirections_from_source(const char *source,
                                                  struct shell_state *state,
                                                  struct redir_vec *redirs);
int simple_command_execute_parts(struct shell_state *state,
                                 const struct ast_word_vec *ast_raw_words,
                                 const struct redir_vec *ast_redirs,
                                 bool allow_builtin,
                                 simple_command_body_runner run_body);
int simple_command_execute(struct shell_state *state, const char *source,
                           bool allow_builtin,
                           simple_command_body_runner run_body);

#endif
