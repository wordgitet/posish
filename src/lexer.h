/* SPDX-License-Identifier: 0BSD */

/* posish - lexer interface */

#ifndef POSISH_LEXER_H
#define POSISH_LEXER_H

#include <stddef.h>

struct token_vec {
    char **items;
    size_t len;
};

int lexer_split_words(const char *line, struct token_vec *out);
int lexer_split_words_at(const char *source_name, const char *line,
                         size_t base_line, struct token_vec *out);
void lexer_free_tokens(struct token_vec *tokens);

#endif
