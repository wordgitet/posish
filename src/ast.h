/* SPDX-License-Identifier: 0BSD */

/* posish - ast definitions */

#ifndef POSISH_AST_H
#define POSISH_AST_H

#include "redir.h"

#include <stdbool.h>
#include <stddef.h>

struct ast_span {
    size_t start_offset;
    size_t end_offset;
    size_t start_line;
    size_t start_col;
    size_t end_line;
    size_t end_col;
};

enum ast_node_kind {
    AST_NODE_EMPTY,
    AST_NODE_SEQUENCE,
    AST_NODE_ASYNC,
    AST_NODE_AND_OR,
    AST_NODE_PIPELINE,
    AST_NODE_SIMPLE_COMMAND,
    AST_NODE_SUBSHELL,
    AST_NODE_BRACE_GROUP,
    AST_NODE_IF,
    AST_NODE_WHILE,
    AST_NODE_UNTIL,
    AST_NODE_FOR,
    AST_NODE_FUNCTION_DEF,
    AST_NODE_CASE,
    AST_NODE_LEGACY
};

enum ast_andor_op {
    AST_ANDOR_AND,
    AST_ANDOR_OR
};

enum ast_case_clause_term {
    AST_CASE_TERM_END,
    AST_CASE_TERM_DBL_SEMI,
    AST_CASE_TERM_SEMI_AMP,
    AST_CASE_TERM_DBL_SEMI_AMP
};

struct ast_word_vec {
    char **items;
    size_t len;
};

struct ast_node;

struct ast_case_clause {
    char *patterns;
    char *body;
    struct ast_node *body_node;
    enum ast_case_clause_term terminator;
};

struct ast_node {
    enum ast_node_kind kind;
    struct ast_span span;
    char *source;
    union {
        struct {
            struct ast_node **items;
            size_t len;
        } list;
        struct {
            struct ast_node *child;
        } unary;
        struct {
            struct ast_node **items;
            enum ast_andor_op *ops;
            size_t len;
        } andor;
        struct {
            struct ast_node **items;
            size_t len;
            bool negate;
        } pipeline;
        struct {
            struct ast_word_vec raw_words;
            struct redir_vec redirs;
        } simple;
        struct {
            char *body;
            struct ast_node *body_node;
            char *redir_suffix;
        } group;
        struct {
            char *cond;
            struct ast_node *cond_node;
            char *then_part;
            struct ast_node *then_node;
            char *else_part;
            struct ast_node *else_node;
            char *redir_suffix;
        } if_cmd;
        struct {
            char *cond;
            struct ast_node *cond_node;
            char *body;
            struct ast_node *body_node;
            char *redir_suffix;
        } loop;
        struct {
            char *name;
            char *words;
            char *body;
            struct ast_node *body_node;
            char *redir_suffix;
            bool implicit_words;
        } for_cmd;
        struct {
            char *name;
            char *body;
            struct ast_node *body_node;
        } funcdef;
        struct {
            char *word_expr;
            struct ast_case_clause *clauses;
            size_t clause_count;
            char *redir_suffix;
        } case_cmd;
    } data;
};

struct ast_program {
    char *source;
    struct ast_node *root;
};

void ast_program_free(struct ast_program *program);

#endif
