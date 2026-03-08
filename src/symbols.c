/* SPDX-License-Identifier: 0BSD */

/* posish - shared hashed name tables */

#include "symbols.h"

#include "arena.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static bool symbol_name_equals_n(const char *stored, const char *name,
                                 size_t len) {
    return strlen(stored) == len && stored[0] == name[0] &&
           memcmp(stored, name, len) == 0;
}

size_t symbol_hash_n(const char *name, size_t len) {
    size_t i;
    size_t hash;

    hash = 1469598103934665603ull;
    for (i = 0; i < len; i++) {
        hash ^= (unsigned char)name[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

static void symbol_table_rehash(struct symbol_table *table, size_t bucket_count) {
    struct symbol_node **buckets;
    struct symbol_node *node;

    buckets = arena_alloc_in(NULL, sizeof(*buckets) * bucket_count);
    memset(buckets, 0, sizeof(*buckets) * bucket_count);

    for (node = table->iter_head; node != NULL; node = node->iter_next) {
        size_t bucket;

        bucket = node->hash % bucket_count;
        node->bucket_next = buckets[bucket];
        buckets[bucket] = node;
    }

    free(table->buckets);
    table->buckets = buckets;
    table->bucket_count = bucket_count;
}

void symbol_table_init(struct symbol_table *table) {
    table->buckets = NULL;
    table->bucket_count = 0;
    table->count = 0;
    table->iter_head = NULL;
}

void symbol_table_destroy(struct symbol_table *table,
                          void (*destroy_node)(struct symbol_node *node)) {
    struct symbol_node *node;

    node = table->iter_head;
    while (node != NULL) {
        struct symbol_node *next;

        next = node->iter_next;
        destroy_node(node);
        node = next;
    }

    free(table->buckets);
    table->buckets = NULL;
    table->bucket_count = 0;
    table->count = 0;
    table->iter_head = NULL;
}

struct symbol_node *symbol_table_lookup_n(const struct symbol_table *table,
                                          const char *name, size_t len) {
    size_t bucket;
    size_t hash;
    struct symbol_node *node;

    if (table->bucket_count == 0) {
        return NULL;
    }

    hash = symbol_hash_n(name, len);
    bucket = hash % table->bucket_count;
    for (node = table->buckets[bucket]; node != NULL; node = node->bucket_next) {
        if (node->hash == hash && symbol_name_equals_n(node->name, name, len)) {
            return node;
        }
    }
    return NULL;
}

struct symbol_node *symbol_table_lookup(const struct symbol_table *table,
                                        const char *name) {
    return symbol_table_lookup_n(table, name, strlen(name));
}

int symbol_table_insert(struct symbol_table *table, struct symbol_node *node) {
    size_t bucket;
    struct symbol_node *tail;

    if (table->bucket_count == 0) {
        symbol_table_rehash(table, 64);
    } else if ((table->count + 1) * 4 >= table->bucket_count * 3) {
        symbol_table_rehash(table, table->bucket_count * 2);
    }

    bucket = node->hash % table->bucket_count;
    node->bucket_next = table->buckets[bucket];
    table->buckets[bucket] = node;
    node->iter_next = NULL;
    if (table->iter_head == NULL) {
        table->iter_head = node;
    } else {
        tail = table->iter_head;
        while (tail->iter_next != NULL) {
            tail = tail->iter_next;
        }
        tail->iter_next = node;
    }
    table->count++;
    return 0;
}

struct symbol_node *symbol_table_remove_n(struct symbol_table *table,
                                          const char *name, size_t len) {
    size_t bucket;
    size_t hash;
    struct symbol_node **bucket_link;
    struct symbol_node **iter_link;
    struct symbol_node *node;

    if (table->bucket_count == 0) {
        return NULL;
    }

    hash = symbol_hash_n(name, len);
    bucket = hash % table->bucket_count;

    bucket_link = &table->buckets[bucket];
    while (*bucket_link != NULL) {
        node = *bucket_link;
        if (node->hash == hash && symbol_name_equals_n(node->name, name, len)) {
            *bucket_link = node->bucket_next;

            iter_link = &table->iter_head;
            while (*iter_link != NULL) {
                if (*iter_link == node) {
                    *iter_link = node->iter_next;
                    break;
                }
                iter_link = &(*iter_link)->iter_next;
            }

            node->bucket_next = NULL;
            node->iter_next = NULL;
            table->count--;
            return node;
        }
        bucket_link = &node->bucket_next;
    }

    return NULL;
}

struct symbol_node *symbol_table_remove(struct symbol_table *table,
                                        const char *name) {
    return symbol_table_remove_n(table, name, strlen(name));
}

struct symbol_node *symbol_table_first(const struct symbol_table *table) {
    return table->iter_head;
}

struct symbol_node *symbol_table_next(const struct symbol_node *node) {
    return node->iter_next;
}
