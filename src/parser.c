/* SPDX-License-Identifier: 0BSD */

/* posish - parser entrypoints */

#include "parser.h"

#include "arena.h"

int parse_program(const char *source, struct ast_program **out_program) {
    struct ast_program *program;

    program = arena_xmalloc(sizeof(*program));
    program->source = arena_xstrdup(source);

    *out_program = program;
    return 0;
}

void ast_program_free(struct ast_program *program) {
    (void)program;
    /*
     * AST program objects are allocated from command/script arenas and are
     * released via arena mark-rewind/reset at the execution boundary.
     */
}
