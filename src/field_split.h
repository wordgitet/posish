/* SPDX-License-Identifier: 0BSD */

/* posish - field splitting helpers */

#ifndef POSISH_FIELD_SPLIT_H
#define POSISH_FIELD_SPLIT_H

#include "shell.h"
#include "lexer.h"

#include <stdbool.h>

void field_split_restore_quoted_markers(char *s);
bool field_split_has_delimiter(const char *expanded,
                               const struct shell_state *state);
bool field_split_has_at_marker(const char *expanded);
int field_split_split(const char *expanded, struct token_vec *out,
                      const struct shell_state *state);
int field_split_append_piece(char *piece, struct token_vec *out,
                             bool split_fields,
                             const struct shell_state *state);
int field_split_append_at_expansion(const char *expanded, struct token_vec *out,
                                    bool split_fields,
                                    const struct shell_state *state);

#endif
