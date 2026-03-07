/* SPDX-License-Identifier: 0BSD */

/* posish - path utilities */

#include "path.h"

#include "arena.h"
#include "shell.h"
#include "vars.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char *path_strdup_heap(const char *s) {
    return arena_strdup_in(NULL, s);
}

static char *path_duplicate_with_cwd(const char *name) {
    char *cwd;
    char *absolute;
    size_t clen;
    size_t nlen;

    if (name[0] == '/') {
        return path_strdup_heap(name);
    }

    cwd = path_getcwd_alloc();
    if (cwd == NULL) {
        return path_strdup_heap(name);
    }

    clen = strlen(cwd);
    nlen = strlen(name);
    absolute = arena_alloc_in(NULL, clen + 1 + nlen + 1);
    memcpy(absolute, cwd, clen);
    absolute[clen] = '/';
    memcpy(absolute + clen + 1, name, nlen + 1);
    arena_maybe_free(cwd);
    return absolute;
}

static void path_cache_remove_at(struct shell_state *state, size_t idx) {
    size_t i;

    arena_maybe_free(state->path_cache[idx].name);
    arena_maybe_free(state->path_cache[idx].path_value);
    arena_maybe_free(state->path_cache[idx].resolved_path);
    for (i = idx + 1; i < state->path_cache_count; i++) {
        state->path_cache[i - 1] = state->path_cache[i];
    }
    state->path_cache_count--;
}

static char *path_cache_hit(struct shell_state *state, const char *name,
                            const char *path_value, bool use_standard_path) {
    size_t i;

    for (i = 0; i < state->path_cache_count; i++) {
        struct command_path_cache_entry *entry;

        entry = &state->path_cache[i];
        if (entry->use_standard_path != use_standard_path) {
            continue;
        }
        if (strcmp(entry->name, name) != 0 ||
            strcmp(entry->path_value, path_value) != 0) {
            continue;
        }
        if (access(entry->resolved_path, X_OK) != 0) {
            path_cache_remove_at(state, i);
            return NULL;
        }
        return path_strdup_heap(entry->resolved_path);
    }

    return NULL;
}

static void path_cache_store(struct shell_state *state, const char *name,
                             const char *path_value, const char *resolved_path,
                             bool use_standard_path) {
    size_t i;
    struct command_path_cache_entry *entry;

    for (i = 0; i < state->path_cache_count; i++) {
        entry = &state->path_cache[i];
        if (entry->use_standard_path != use_standard_path) {
            continue;
        }
        if (strcmp(entry->name, name) != 0 ||
            strcmp(entry->path_value, path_value) != 0) {
            continue;
        }
        arena_maybe_free(entry->resolved_path);
        entry->resolved_path = path_strdup_heap(resolved_path);
        return;
    }

    state->path_cache = arena_realloc_in(
        NULL, state->path_cache,
        sizeof(*state->path_cache) * (state->path_cache_count + 1));
    entry = &state->path_cache[state->path_cache_count++];
    entry->name = path_strdup_heap(name);
    entry->path_value = path_strdup_heap(path_value);
    entry->resolved_path = path_strdup_heap(resolved_path);
    entry->use_standard_path = use_standard_path;
}

static char *path_search_command_in_path(const char *name, const char *path) {
    const char *p;

    if (path == NULL || path[0] == '\0') {
        return NULL;
    }

    p = path;
    while (1) {
        const char *end;
        size_t dlen;
        const char *dir;
        char *candidate;

        end = strchr(p, ':');
        if (end == NULL) {
            end = p + strlen(p);
        }
        dlen = (size_t)(end - p);
        dir = dlen == 0 ? "." : p;

        candidate = arena_alloc_in(NULL,
                                   (dlen == 0 ? 1 : dlen) + 1 + strlen(name) + 1);
        if (dlen == 0) {
            strcpy(candidate, ".");
        } else {
            memcpy(candidate, dir, dlen);
            candidate[dlen] = '\0';
        }
        strcat(candidate, "/");
        strcat(candidate, name);

        if (access(candidate, X_OK) == 0) {
            return candidate;
        }
        arena_maybe_free(candidate);

        if (*end == '\0') {
            break;
        }
        p = end + 1;
    }

    return NULL;
}

char *path_getcwd_alloc(void) {
    size_t size;
    char *buf;
    char *result;

    size = 128;
    buf = arena_alloc_in(NULL, size);
    for (;;) {
        if (getcwd(buf, size) != NULL) {
            result = arena_xstrdup(buf);
            arena_maybe_free(buf);
            return result;
        }

        if (errno != ERANGE) {
            arena_maybe_free(buf);
            return NULL;
        }
        if (size > SIZE_MAX / 2) {
            arena_maybe_free(buf);
            errno = ERANGE;
            return NULL;
        }
        size *= 2;
        buf = arena_realloc_in(NULL, buf, size);
    }
}

void path_cache_invalidate(struct shell_state *state) {
    size_t i;

    for (i = 0; i < state->path_cache_count; i++) {
        arena_maybe_free(state->path_cache[i].name);
        arena_maybe_free(state->path_cache[i].path_value);
        arena_maybe_free(state->path_cache[i].resolved_path);
    }
    arena_maybe_free(state->path_cache);
    state->path_cache = NULL;
    state->path_cache_count = 0;
}

void path_cache_destroy(struct shell_state *state) { path_cache_invalidate(state); }

char *path_resolve_command(struct shell_state *state, const char *name,
                           bool use_standard_path) {
    const char *path_value;
    char *cached;
    char *resolved;

    if (name == NULL || name[0] == '\0') {
        return NULL;
    }

    if (strchr(name, '/') != NULL) {
        if (access(name, X_OK) != 0) {
            return NULL;
        }
        return path_duplicate_with_cwd(name);
    }

    path_value = use_standard_path ? "/bin:/usr/bin" : vars_get(state, "PATH");
    if (path_value == NULL || path_value[0] == '\0') {
        errno = ENOENT;
        return NULL;
    }

    cached = path_cache_hit(state, name, path_value, use_standard_path);
    if (cached != NULL) {
        return cached;
    }

    resolved = path_search_command_in_path(name, path_value);
    if (resolved == NULL) {
        errno = ENOENT;
        return NULL;
    }
    path_cache_store(state, name, path_value, resolved, use_standard_path);
    return resolved;
}

bool path_resolves_command(struct shell_state *state, const char *name,
                           bool use_standard_path) {
    char *resolved;

    resolved = path_resolve_command(state, name, use_standard_path);
    if (resolved == NULL) {
        return false;
    }
    arena_maybe_free(resolved);
    return true;
}

char *path_resolve_dot_script(struct shell_state *state, const char *name) {
    const char *path;
    const char *p;

    if (strchr(name, '/') != NULL) {
        if (access(name, R_OK) == 0) {
            return path_strdup_heap(name);
        }
        return NULL;
    }

    path = vars_get(state, "PATH");
    if (path == NULL || path[0] == '\0') {
        path = ".";
    }
    p = path;

    while (1) {
        const char *colon;
        size_t dir_len;
        const char *dir;
        size_t name_len;
        char *candidate;

        colon = strchr(p, ':');
        dir_len = colon == NULL ? strlen(p) : (size_t)(colon - p);
        dir = p;
        if (dir_len == 0) {
            dir = ".";
            dir_len = 1;
        }

        name_len = strlen(name);
        candidate = arena_alloc_in(NULL, dir_len + 1 + name_len + 1);
        memcpy(candidate, dir, dir_len);
        candidate[dir_len] = '/';
        memcpy(candidate + dir_len + 1, name, name_len + 1);

        if (access(candidate, R_OK) == 0) {
            return candidate;
        }
        arena_maybe_free(candidate);

        if (colon == NULL) {
            break;
        }
        p = colon + 1;
    }

    return NULL;
}
