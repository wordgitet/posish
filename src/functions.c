/* SPDX-License-Identifier: 0BSD */

/* posish - shell function table */

#include "functions.h"

#include "arena.h"
#include "parser.h"
#include "symbols.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

struct shell_function_entry {
    struct symbol_node sym;
    struct shell_function function;
};

static struct shell_function_entry *function_entry_from_node(struct symbol_node *node) {
    return (struct shell_function_entry *)((char *)node -
                                           offsetof(struct shell_function_entry, sym));
}

static const struct shell_function_entry *
function_entry_from_const_node(const struct symbol_node *node) {
    return (const struct shell_function_entry *)((const char *)node -
                                                 offsetof(struct shell_function_entry, sym));
}

static void function_redirs_clone_heap(struct redir_vec *dst,
                                       const struct redir_vec *src) {
    size_t i;

    dst->items = NULL;
    dst->len = 0;
    if (src == NULL || src->len == 0) {
        return;
    }

    dst->items = heap_xmalloc(sizeof(*dst->items) * src->len);
    dst->len = src->len;
    for (i = 0; i < src->len; i++) {
        dst->items[i] = src->items[i];
        if (src->items[i].path != NULL) {
            dst->items[i].path = heap_xstrdup(src->items[i].path);
        }
        if (src->items[i].delimiter != NULL) {
            dst->items[i].delimiter =
                heap_xstrdup(src->items[i].delimiter);
        }
        if (src->items[i].body_raw != NULL) {
            dst->items[i].body_raw =
                heap_xstrdup(src->items[i].body_raw);
        }
    }
}

static void function_redirs_destroy(struct redir_vec *redirs) {
    size_t i;

    for (i = 0; i < redirs->len; i++) {
        heap_free(redirs->items[i].delimiter);
        heap_free(redirs->items[i].body_raw);
        heap_free(redirs->items[i].path);
    }
    heap_free(redirs->items);
    redirs->items = NULL;
    redirs->len = 0;
}

static void function_destroy_node(struct symbol_node *node) {
    struct shell_function_entry *entry;

    entry = function_entry_from_node(node);
    heap_free(entry->sym.name);
    heap_free(entry->function.body);
    function_redirs_destroy(&entry->function.redirs);
    arena_destroy(&entry->function.cache_arena);
    heap_free(entry);
}

void functions_init(struct shell_state *state) {
    symbol_table_init(&state->functions_table);
}

void functions_destroy(struct shell_state *state) {
    symbol_table_destroy(&state->functions_table, function_destroy_node);
}

bool functions_has(const struct shell_state *state, const char *name) {
    return symbol_table_lookup(&state->functions_table, name) != NULL;
}

const struct shell_function *functions_get(const struct shell_state *state,
                                           const char *name) {
    const struct symbol_node *node;

    node = symbol_table_lookup(&state->functions_table, name);
    if (node == NULL) {
        return NULL;
    }
    return &function_entry_from_const_node(node)->function;
}

struct shell_function *functions_get_mut(struct shell_state *state,
                                         const char *name) {
    struct symbol_node *node;

    node = symbol_table_lookup(&state->functions_table, name);
    if (node == NULL) {
        return NULL;
    }
    return &function_entry_from_node(node)->function;
}

const struct ast_program *functions_get_cached_program(struct shell_state *state,
                                                       struct shell_function *function) {
    struct ast_program *program;
    struct arena *saved_arena;

    if (function == NULL) {
        return NULL;
    }
    if (function->cached_program != NULL) {
        return function->cached_program;
    }
    if (function->cache_attempted) {
        return NULL;
    }

    program = NULL;
    saved_arena = arena_get_current();
    arena_set_current(&function->cache_arena);
    if (parse_program_at(state->current_source_name, state->current_source_base_line,
                         function->body, &program) != 0) {
        arena_reset(&function->cache_arena);
        program = NULL;
    }
    arena_set_current(saved_arena);

    function->cached_program = program;
    function->cache_attempted = true;
    return function->cached_program;
}

int functions_set(struct shell_state *state, const char *name, const char *body,
                  const struct redir_vec *redirs) {
    struct shell_function_entry *entry;
    struct symbol_node *node;

    node = symbol_table_lookup(&state->functions_table, name);
    if (node != NULL) {
        entry = function_entry_from_node(node);
        heap_free(entry->function.body);
        function_redirs_destroy(&entry->function.redirs);
        arena_reset(&entry->function.cache_arena);
        entry->function.body = heap_xstrdup(body);
        function_redirs_clone_heap(&entry->function.redirs, redirs);
        entry->function.cached_program = NULL;
        entry->function.cache_attempted = false;
        return 0;
    }

    entry = heap_xmalloc(sizeof(*entry));
    memset(entry, 0, sizeof(*entry));
    entry->sym.name = heap_xstrdup(name);
    entry->sym.hash = symbol_hash_n(name, strlen(name));
    entry->function.body = heap_xstrdup(body);
    function_redirs_clone_heap(&entry->function.redirs, redirs);
    arena_init(&entry->function.cache_arena, 4096);
    entry->function.cached_program = NULL;
    entry->function.cache_attempted = false;
    return symbol_table_insert(&state->functions_table, &entry->sym);
}

int functions_remove(struct shell_state *state, const char *name) {
    struct symbol_node *node;

    node = symbol_table_remove(&state->functions_table, name);
    if (node != NULL) {
        function_destroy_node(node);
    }
    return 0;
}
