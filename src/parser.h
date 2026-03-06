/* SPDX-License-Identifier: 0BSD */

/* posish - parser interface */

#ifndef POSISH_PARSER_H
#define POSISH_PARSER_H

#include "ast.h"

#include <stddef.h>

int parse_program(const char *source, struct ast_program **out_program);
int parse_program_at(const char *source_name, size_t base_line,
                     const char *source, struct ast_program **out_program);

#endif
