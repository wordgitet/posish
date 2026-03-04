/* SPDX-License-Identifier: 0BSD */

/* posish - arena allocator */

#include "arena.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct arena_block {
    struct arena_block *next;
    size_t used;
    size_t cap;
    max_align_t align_pad;
    unsigned char data[];
};

struct arena_registry_entry {
    struct arena *arena;
    struct arena_registry_entry *next;
};

struct arena_alloc_header {
    uint32_t magic;
    uint32_t reserved;
    size_t size;
    struct arena *owner;
};

union arena_alloc_slot {
    struct arena_alloc_header h;
    max_align_t align;
};

#define ARENA_ALLOC_MAGIC 0x50534152u /* "PSAR" */
#define ARENA_MIN_BLOCK_SIZE (64u * 1024u)

static struct arena *g_current_arena = NULL;
static struct arena_registry_entry *g_registry = NULL;
static int g_arena_strict_mode = -1;

static size_t align_up(size_t value, size_t alignment);
static struct arena_block *arena_new_block(size_t capacity);
static void arena_release_blocks(struct arena_block *head);
static void arena_recycle_block(struct arena *arena, struct arena_block *block);
static struct arena_block *arena_take_recycled_block(struct arena *arena,
                                                     size_t min_capacity);
static void arena_register(struct arena *arena);
static void arena_unregister(struct arena *arena);
static bool arena_pointer_in_block(const struct arena_block *block, const void *ptr);
static union arena_alloc_slot *arena_ptr_header(void *ptr);
static bool arena_strict_mode_enabled(void);

static size_t align_up(size_t value, size_t alignment) {
    size_t mask;

    mask = alignment - 1;
    return (value + mask) & ~mask;
}

static struct arena_block *arena_new_block(size_t capacity) {
    struct arena_block *block;

    block = malloc(sizeof(*block) + capacity);
    if (block == NULL) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    block->next = NULL;
    block->used = 0;
    block->cap = capacity;
    return block;
}

static void arena_release_blocks(struct arena_block *head) {
    while (head != NULL) {
        struct arena_block *next;

        next = head->next;
        free(head);
        head = next;
    }
}

static void arena_recycle_block(struct arena *arena, struct arena_block *block) {
    block->used = 0;
    block->next = arena->free_list;
    arena->free_list = block;
}

static struct arena_block *arena_take_recycled_block(struct arena *arena,
                                                     size_t min_capacity) {
    struct arena_block **link;

    link = &arena->free_list;
    while (*link != NULL) {
        struct arena_block *block;

        block = *link;
        if (block->cap >= min_capacity) {
            *link = block->next;
            block->next = NULL;
            block->used = 0;
            return block;
        }
        link = &block->next;
    }
    return NULL;
}

static void arena_register(struct arena *arena) {
    struct arena_registry_entry *entry;

    entry = malloc(sizeof(*entry));
    if (entry == NULL) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }
    entry->arena = arena;
    entry->next = g_registry;
    g_registry = entry;
}

static void arena_unregister(struct arena *arena) {
    struct arena_registry_entry **link;

    link = &g_registry;
    while (*link != NULL) {
        if ((*link)->arena == arena) {
            struct arena_registry_entry *dead;

            dead = *link;
            *link = dead->next;
            free(dead);
            return;
        }
        link = &(*link)->next;
    }
}

static bool arena_pointer_in_block(const struct arena_block *block, const void *ptr) {
    const unsigned char *p;
    const unsigned char *start;
    const unsigned char *end;

    if (ptr == NULL) {
        return false;
    }

    p = (const unsigned char *)ptr;
    start = block->data;
    /*
     * Ownership checks must cover full block capacity so pointers from
     * rewound/reset regions are still recognized as arena-owned and never
     * passed to free(3).
     */
    end = block->data + block->cap;
    return p >= start && p < end;
}

static union arena_alloc_slot *arena_ptr_header(void *ptr) {
    union arena_alloc_slot *slot;

    slot = (union arena_alloc_slot *)ptr;
    return slot - 1;
}

static bool arena_strict_mode_enabled(void) {
    const char *env;

    if (g_arena_strict_mode >= 0) {
        return g_arena_strict_mode != 0;
    }

    env = getenv("POSISH_ARENA_STRICT");
    if (env == NULL || env[0] == '\0' || strcmp(env, "0") == 0) {
        g_arena_strict_mode = 0;
        return false;
    }

    g_arena_strict_mode = 1;
    return true;
}

void arena_init(struct arena *arena, size_t block_size) {
    if (block_size < ARENA_MIN_BLOCK_SIZE) {
        block_size = ARENA_MIN_BLOCK_SIZE;
    }

    arena->head = NULL;
    arena->free_list = NULL;
    arena->default_block_size = block_size;
    arena_register(arena);
}

void arena_destroy(struct arena *arena) {
    arena_release_blocks(arena->head);
    arena_release_blocks(arena->free_list);
    arena->head = NULL;
    arena->free_list = NULL;
    if (g_current_arena == arena) {
        g_current_arena = NULL;
    }
    arena_unregister(arena);
    arena->default_block_size = 0;
}

void arena_reset(struct arena *arena) {
    while (arena->head != NULL) {
        struct arena_block *block;

        block = arena->head;
        arena->head = block->next;
        arena_recycle_block(arena, block);
    }
}

void arena_mark_take(const struct arena *arena, struct arena_mark *mark) {
    mark->block = arena->head;
    mark->used = arena->head != NULL ? arena->head->used : 0;
}

void arena_mark_rewind(struct arena *arena, const struct arena_mark *mark) {
    while (arena->head != NULL && arena->head != mark->block) {
        struct arena_block *dead;

        dead = arena->head;
        arena->head = dead->next;
        arena_recycle_block(arena, dead);
    }

    if (arena->head == NULL) {
        return;
    }

    if (mark->used <= arena->head->used) {
        arena->head->used = mark->used;
    }
}

void arena_set_current(struct arena *arena) { g_current_arena = arena; }

struct arena *arena_get_current(void) { return g_current_arena; }

void *arena_alloc_in(struct arena *arena, size_t size) {
    size_t payload_size;
    size_t total_size;
    size_t alignment;
    size_t block_needed;
    size_t block_size;
    struct arena_block *block;
    unsigned char *base;
    union arena_alloc_slot *slot;

    if (arena == NULL) {
        void *ptr;

        payload_size = size == 0 ? 1 : size;
        ptr = malloc(payload_size);
        if (ptr == NULL) {
            perror("malloc");
            exit(EXIT_FAILURE);
        }
        return ptr;
    }

    payload_size = size == 0 ? 1 : size;
    alignment = sizeof(max_align_t);
    total_size = sizeof(union arena_alloc_slot);
    total_size = align_up(total_size, alignment);
    total_size += align_up(payload_size, alignment);

    block = arena->head;
    if (block == NULL || block->used + total_size > block->cap) {
        block_needed = total_size;
        block = arena_take_recycled_block(arena, block_needed);
        if (block == NULL) {
            block_size = arena->default_block_size;
            while (block_size < block_needed) {
                block_size *= 2;
            }
            block = arena_new_block(block_size);
        }
        block->next = arena->head;
        arena->head = block;
    }

    base = block->data + block->used;
    slot = (union arena_alloc_slot *)base;
    slot->h.magic = ARENA_ALLOC_MAGIC;
    slot->h.reserved = 0;
    slot->h.size = payload_size;
    slot->h.owner = arena;
    block->used += total_size;
    return (void *)(slot + 1);
}

void *arena_realloc_in(struct arena *arena, void *ptr, size_t size) {
    size_t payload_size;

    if (ptr == NULL) {
        return arena_alloc_in(arena, size);
    }

    payload_size = size == 0 ? 1 : size;
    if (arena == NULL) {
        void *re;

        re = realloc(ptr, payload_size);
        if (re == NULL) {
            perror("realloc");
            exit(EXIT_FAILURE);
        }
        return re;
    }

    {
        union arena_alloc_slot *old_slot;
        void *new_ptr;
        size_t copy_size;

        old_slot = arena_ptr_header(ptr);
        if (old_slot->h.magic != ARENA_ALLOC_MAGIC || old_slot->h.owner == NULL) {
            if (arena_strict_mode_enabled()) {
                fprintf(stderr,
                        "posish: arena strict mode: non-arena pointer passed to arena realloc\n");
                abort();
            }
            /*
             * Compatibility path while migrating callers: if a non-arena pointer
             * reaches arena realloc, keep behavior safe instead of crashing.
             */
            void *re;

            re = realloc(ptr, payload_size);
            if (re == NULL) {
                perror("realloc");
                exit(EXIT_FAILURE);
            }
            return re;
        }

        new_ptr = arena_alloc_in(arena, payload_size);
        copy_size = old_slot->h.size < payload_size ? old_slot->h.size : payload_size;
        memcpy(new_ptr, ptr, copy_size);
        return new_ptr;
    }
}

char *arena_strdup_in(struct arena *arena, const char *s) {
    size_t len;
    char *copy;

    len = strlen(s);
    copy = arena_alloc_in(arena, len + 1);
    memcpy(copy, s, len + 1);
    return copy;
}

bool arena_owns_pointer(const void *ptr) {
    const struct arena_registry_entry *entry;

    if (ptr == NULL) {
        return false;
    }

    for (entry = g_registry; entry != NULL; entry = entry->next) {
        const struct arena_block *block;

        for (block = entry->arena->head; block != NULL; block = block->next) {
            if (arena_pointer_in_block(block, ptr)) {
                return true;
            }
        }
        for (block = entry->arena->free_list; block != NULL; block = block->next) {
            if (arena_pointer_in_block(block, ptr)) {
                return true;
            }
        }
    }
    return false;
}

void arena_maybe_free(void *ptr) {
    if (ptr == NULL) {
        return;
    }
    if (arena_owns_pointer(ptr)) {
        return;
    }
    free(ptr);
}

void *arena_xmalloc(size_t size) { return arena_alloc_in(g_current_arena, size); }

void *arena_xrealloc(void *ptr, size_t size) {
    return arena_realloc_in(g_current_arena, ptr, size);
}

char *arena_xstrdup(const char *s) { return arena_strdup_in(g_current_arena, s); }
