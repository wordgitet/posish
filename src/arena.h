/* SPDX-License-Identifier: 0BSD */

/* posish - allocator interface */

#ifndef POSISH_ARENA_H
#define POSISH_ARENA_H

#include <stdbool.h>
#include <stddef.h>

struct arena_block;

struct arena {
    struct arena_block *head;
    struct arena_block *free_list;
    size_t default_block_size;
};

struct arena_mark {
    struct arena_block *block;
    size_t used;
};

void arena_init(struct arena *arena, size_t block_size);
void arena_destroy(struct arena *arena);
void arena_reset(struct arena *arena);
void arena_mark_take(const struct arena *arena, struct arena_mark *mark);
void arena_mark_rewind(struct arena *arena, const struct arena_mark *mark);
void arena_set_current(struct arena *arena);
struct arena *arena_get_current(void);
void *arena_alloc_in(struct arena *arena, size_t size);
void *arena_realloc_in(struct arena *arena, void *ptr, size_t size);
char *arena_strdup_in(struct arena *arena, const char *s);
bool arena_owns_pointer(const void *ptr);
void arena_maybe_free(void *ptr);

void *arena_xmalloc(size_t size);
void *arena_xrealloc(void *ptr, size_t size);
char *arena_xstrdup(const char *s);

#endif
