#include "parser.h"

#include "arena.h"
#include "error.h"

#include <stdlib.h>

static int reject_dollar_single_quotes(const char *source) {
    size_t i;
    int quote;

    quote = 0;
    i = 0;
    while (source[i] != '\0') {
        char ch;

        ch = source[i];

        if (quote == 0) {
            if (ch == '\'') {
                quote = '\'';
                i++;
                continue;
            }
            if (ch == '"') {
                quote = '"';
                i++;
                continue;
            }
            if (ch == '\\' && source[i + 1] != '\0') {
                i += 2;
                continue;
            }
            if (ch == '$' && source[i + 1] == '\'') {
                size_t j;
                size_t line;
                size_t col;

                line = 1;
                col = 1;
                for (j = 0; j < i; j++) {
                    if (source[j] == '\n') {
                        line++;
                        col = 1;
                    } else {
                        col++;
                    }
                }
                posish_error_at("<input>", line, col, "syntax",
                                "dollar-single-quote is not implemented");
                return -1;
            }
            i++;
            continue;
        }

        if (quote == '\'' && ch == '\'') {
            quote = 0;
            i++;
            continue;
        }

        if (quote == '"') {
            if (ch == '\\' && source[i + 1] != '\0') {
                i += 2;
                continue;
            }
            if (ch == '"') {
                quote = 0;
                i++;
                continue;
            }
        }

        i++;
    }

    return 0;
}

int parse_program(const char *source, struct ast_program **out_program) {
    struct ast_program *program;

    if (reject_dollar_single_quotes(source) != 0) {
        return -1;
    }

    program = arena_xmalloc(sizeof(*program));
    program->source = arena_xstrdup(source);

    *out_program = program;
    return 0;
}

void ast_program_free(struct ast_program *program) {
    if (program == NULL) {
        return;
    }

    free(program->source);
    free(program);
}
