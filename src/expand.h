/* SPDX-License-Identifier: 0BSD */

/* posish - expansion interface */

#ifndef POSISH_EXPAND_H
#define POSISH_EXPAND_H

#include "lexer.h"
#include "shell.h"

int expand_words(const struct token_vec *in, struct token_vec *out,
                 struct shell_state *state, bool split_fields);

#endif
