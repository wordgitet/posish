/* SPDX-License-Identifier: 0BSD */

/* posish - shared hashed name tables */

#ifndef POSISH_SYMBOLS_H
#define POSISH_SYMBOLS_H

#include <stddef.h>

struct symbol_node {
    struct symbol_node *bucket_next;
    struct symbol_node *iter_next;
    size_t hash;
    char *name;
};

struct symbol_table {
    struct symbol_node **buckets;
    size_t bucket_count;
    size_t count;
    struct symbol_node *iter_head;
};

void symbol_table_init(struct symbol_table *table);
void symbol_table_destroy(struct symbol_table *table,
                          void (*destroy_node)(struct symbol_node *node));

size_t symbol_hash_n(const char *name, size_t len);

struct symbol_node *symbol_table_lookup(const struct symbol_table *table,
                                        const char *name);
struct symbol_node *symbol_table_lookup_n(const struct symbol_table *table,
                                          const char *name, size_t len);

int symbol_table_insert(struct symbol_table *table, struct symbol_node *node);
struct symbol_node *symbol_table_remove(struct symbol_table *table,
                                        const char *name);
struct symbol_node *symbol_table_remove_n(struct symbol_table *table,
                                          const char *name, size_t len);

struct symbol_node *symbol_table_first(const struct symbol_table *table);
struct symbol_node *symbol_table_next(const struct symbol_node *node);

#endif
