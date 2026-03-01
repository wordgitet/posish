#ifndef POSISH_COMPOUND_PARSE_H
#define POSISH_COMPOUND_PARSE_H

#include <stdbool.h>

bool parse_simple_if(const char *source, char **cond_out, char **then_out,
                     char **else_out);
bool parse_simple_while(const char *source, char **cond_out, char **body_out,
                        bool *is_until_out);
bool parse_simple_for(const char *source, char **name_out, char **words_out,
                      char **body_out);
bool compound_needs_single_atom(const char *source);

#endif
