#ifndef POSISH_PARSER_H
#define POSISH_PARSER_H

#include "ast.h"

int parse_program(const char *source, struct ast_program **out_program);

#endif
