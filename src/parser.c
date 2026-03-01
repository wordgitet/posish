/* SPDX-License-Identifier: 0BSD */

/* posish - parser entrypoints */

#include "parser.h"

#include "arena.h"
#include <stdlib.h>

int parse_program(const char *source, struct ast_program **out_program) {
    struct ast_program *program;

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
