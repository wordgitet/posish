#ifndef POSISH_AST_H
#define POSISH_AST_H

struct ast_program {
    char *source;
};

void ast_program_free(struct ast_program *program);

#endif
