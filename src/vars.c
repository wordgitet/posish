/* SPDX-License-Identifier: 0BSD */

/* posish - variable storage */

#include "vars.h"

#include "arena.h"
#include "error.h"
#include "path.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

extern char **environ;

static bool vars_is_process_marker(const char *name) {
    return strcmp(name, "POSISH_PARENT_INTERACTIVE") == 0 ||
           strcmp(name, "POSISH_TRACE") == 0 ||
           strcmp(name, "POSISH_LINENO_BASE") == 0;
}

static bool vars_should_skip_import(const char *name) {
    static const char alias_prefix[] = "POSISH_ALIAS_";

    return vars_is_process_marker(name) ||
           strncmp(name, alias_prefix, sizeof(alias_prefix) - 1) == 0;
}

static bool vars_is_storage_name(const char *name) {
    return strcmp(name, "0") == 0 || vars_is_name_valid(name);
}

static bool vars_is_name_valid_n(const char *name, size_t len) {
    size_t i;

    if (name == NULL || len == 0) {
        return false;
    }
    if (!(isalpha((unsigned char)name[0]) || name[0] == '_')) {
        return false;
    }
    for (i = 1; i < len; i++) {
        if (!(isalnum((unsigned char)name[i]) || name[i] == '_')) {
            return false;
        }
    }
    return true;
}

static bool vars_name_equals_n(const char *stored, const char *name, size_t len) {
    return stored[0] == name[0] && stored[len] == '\0' &&
           memcmp(stored, name, len) == 0;
}

static void vars_cache_store(struct shell_state *state, size_t idx) {
    state->var_mru_valid = true;
    state->var_mru_index = idx;
}

static void vars_cache_invalidate(struct shell_state *state) {
    state->var_mru_valid = false;
    state->var_mru_index = 0;
}

static ssize_t vars_find_n(struct shell_state *state, const char *name,
                           size_t len) {
    size_t i;

    if (state->var_mru_valid && state->var_mru_index < state->var_count &&
        vars_name_equals_n(state->vars[state->var_mru_index].name, name, len)) {
        return (ssize_t)state->var_mru_index;
    }

    /*
     * Benchmarks mostly hammer variables created after startup (for example
     * __count, loop temporaries, PATH overrides). Those entries sit near the
     * end of the table because the inherited environment is imported first.
     * Searching newest-first keeps the common hot path close to O(1) without
     * changing external behavior or adding a larger indexing structure yet.
     */
    for (i = state->var_count; i > 0; i--) {
        const struct shell_var *var;

        var = &state->vars[i - 1];
        if (vars_name_equals_n(var->name, name, len)) {
            vars_cache_store(state, i - 1);
            return (ssize_t)(i - 1);
        }
    }
    return -1;
}

static ssize_t vars_find(struct shell_state *state, const char *name) {
    return vars_find_n(state, name, strlen(name));
}

static struct shell_var *vars_lookup_mut_n(struct shell_state *state,
                                           const char *name, size_t len) {
    ssize_t idx;

    idx = vars_find_n(state, name, len);
    if (idx < 0) {
        return NULL;
    }
    return &state->vars[idx];
}

static struct shell_var *vars_lookup_mut(struct shell_state *state,
                                         const char *name) {
    return vars_lookup_mut_n(state, name, strlen(name));
}

static const struct shell_var *vars_lookup_n(struct shell_state *state,
                                             const char *name, size_t len) {
    ssize_t idx;

    idx = vars_find_n(state, name, len);
    if (idx < 0) {
        return NULL;
    }
    return &state->vars[idx];
}

static const struct shell_var *vars_lookup(struct shell_state *state,
                                           const char *name) {
    return vars_lookup_n(state, name, strlen(name));
}

static bool vars_affects_path_cache(const char *name) {
    return strcmp(name, "PATH") == 0 || strcmp(name, "PWD") == 0;
}

static bool vars_affects_path_cache_n(const char *name, size_t len) {
    return (len == 4 && memcmp(name, "PATH", 4) == 0) ||
           (len == 3 && memcmp(name, "PWD", 3) == 0);
}

static void vars_maybe_invalidate_path_cache(struct shell_state *state,
                                             const char *name) {
    if (vars_affects_path_cache(name)) {
        path_cache_invalidate(state);
    }
}

static void vars_maybe_invalidate_path_cache_n(struct shell_state *state,
                                               const char *name, size_t len) {
    if (vars_affects_path_cache_n(name, len)) {
        path_cache_invalidate(state);
    }
}

static bool vars_parse_long_value(const char *value, long *out) {
    char *end;
    long parsed;

    if (value == NULL || value[0] == '\0') {
        return false;
    }

    errno = 0;
    parsed = strtol(value, &end, 0);
    if (end == value || *end != '\0' || errno == ERANGE) {
        return false;
    }

    *out = parsed;
    return true;
}

static size_t vars_format_long_decimal(long value, char buf[static 3 + sizeof(long) * 3]) {
    unsigned long magnitude;
    size_t pos;
    bool negative;

    negative = value < 0;
    if (negative) {
        magnitude = (unsigned long)(-(value + 1)) + 1;
    } else {
        magnitude = (unsigned long)value;
    }

    pos = 0;
    do {
        buf[pos++] = (char)('0' + (magnitude % 10));
        magnitude /= 10;
    } while (magnitude != 0);

    if (negative) {
        buf[pos++] = '-';
    }

    {
        size_t i;
        for (i = 0; i < pos / 2; i++) {
            char tmp;

            tmp = buf[i];
            buf[i] = buf[pos - 1 - i];
            buf[pos - 1 - i] = tmp;
        }
    }
    buf[pos] = '\0';
    return pos;
}

static int vars_append_new(struct shell_state *state, const char *name,
                           const char *value, bool exported, bool readonly) {
    struct shell_var *grown;
    struct shell_var *var;
    const char *stored_value;
    long parsed_long;

    grown = arena_realloc_in(NULL, state->vars,
                             sizeof(*state->vars) * (state->var_count + 1));
    state->vars = grown;
    var = &state->vars[state->var_count++];
    var->name = arena_strdup_in(NULL, name);
    stored_value = value == NULL ? "" : value;
    parsed_long = 0;
    var->value = arena_strdup_in(NULL, stored_value);
    var->value_len = strlen(stored_value);
    var->long_cache_valid = vars_parse_long_value(stored_value, &parsed_long);
    var->long_cache = parsed_long;
    var->exported = exported;
    var->readonly = readonly;
    vars_cache_store(state, state->var_count - 1);
    vars_maybe_invalidate_path_cache(state, name);
    return 0;
}

static int vars_append_new_n(struct shell_state *state, const char *name,
                             size_t len, const char *value, bool exported,
                             bool readonly) {
    struct shell_var *grown;
    struct shell_var *var;
    const char *stored_value;
    long parsed_long;

    grown = arena_realloc_in(NULL, state->vars,
                             sizeof(*state->vars) * (state->var_count + 1));
    state->vars = grown;
    var = &state->vars[state->var_count++];
    var->name = arena_alloc_in(NULL, len + 1);
    memcpy(var->name, name, len);
    var->name[len] = '\0';
    stored_value = value == NULL ? "" : value;
    parsed_long = 0;
    var->value = arena_strdup_in(NULL, stored_value);
    var->value_len = strlen(stored_value);
    var->long_cache_valid = vars_parse_long_value(stored_value, &parsed_long);
    var->long_cache = parsed_long;
    var->exported = exported;
    var->readonly = readonly;
    vars_cache_store(state, state->var_count - 1);
    vars_maybe_invalidate_path_cache(state, var->name);
    return 0;
}

static void vars_replace_value(struct shell_var *var, const char *value) {
    size_t len;
    long parsed_long;

    len = strlen(value == NULL ? "" : value);
    var->value = arena_realloc_in(NULL, var->value, len + 1);
    memcpy(var->value, value == NULL ? "" : value, len + 1);
    var->value_len = len;
    parsed_long = 0;
    var->long_cache_valid = vars_parse_long_value(value == NULL ? "" : value,
                                                  &parsed_long);
    var->long_cache = parsed_long;
}

static void vars_replace_value_long(struct shell_var *var, long value) {
    char buf[3 + sizeof(long) * 3];
    size_t len;

    len = vars_format_long_decimal(value, buf);
    var->value = arena_realloc_in(NULL, var->value, len + 1);
    memcpy(var->value, buf, len + 1);
    var->value_len = len;
    var->long_cache_valid = true;
    var->long_cache = value;
}

static int vars_store(struct shell_state *state, const char *name,
                      const char *value, bool check_readonly, bool exported,
                      bool mark_readonly) {
    struct shell_var *var;

    var = vars_lookup_mut(state, name);
    if (var != NULL) {
        if (check_readonly && var->readonly) {
            posish_errorf("%s: is read-only", name);
            return 1;
        }
        vars_replace_value(var, value);
        var->exported = exported;
        if (mark_readonly) {
            var->readonly = true;
        }
        vars_cache_store(state, (size_t)(var - state->vars));
        vars_maybe_invalidate_path_cache(state, name);
        return 0;
    }

    return vars_append_new(state, name, value, exported, mark_readonly);
}

static int vars_store_n(struct shell_state *state, const char *name, size_t len,
                        const char *value, bool check_readonly, bool exported,
                        bool mark_readonly) {
    struct shell_var *var;
    char *name_copy;

    var = vars_lookup_mut_n(state, name, len);
    if (var != NULL) {
        if (check_readonly && var->readonly) {
            name_copy = arena_alloc_in(NULL, len + 1);
            memcpy(name_copy, name, len);
            name_copy[len] = '\0';
            posish_errorf("%s: is read-only", name_copy);
            arena_maybe_free(name_copy);
            return 1;
        }
        vars_replace_value(var, value);
        var->exported = exported;
        if (mark_readonly) {
            var->readonly = true;
        }
        vars_cache_store(state, (size_t)(var - state->vars));
        vars_maybe_invalidate_path_cache_n(state, name, len);
        return 0;
    }

    return vars_append_new_n(state, name, len, value, exported, mark_readonly);
}

static int vars_mark_readonly_existing(struct shell_state *state,
                                       const char *name) {
    struct shell_var *var;

    var = vars_lookup_mut(state, name);
    if (var == NULL) {
        return vars_append_new(state, name, "", false, true);
    }
    var->readonly = true;
    return 0;
}

void vars_init(struct shell_state *state) {
    size_t i;

    state->vars = NULL;
    state->var_count = 0;
    vars_cache_invalidate(state);

    for (i = 0; environ != NULL && environ[i] != NULL; i++) {
        const char *entry;
        const char *eq;
        size_t nlen;
        char *name;

        entry = environ[i];
        eq = strchr(entry, '=');
        if (eq == NULL) {
            continue;
        }

        nlen = (size_t)(eq - entry);
        name = arena_alloc_in(NULL, nlen + 1);
        memcpy(name, entry, nlen);
        name[nlen] = '\0';
        if (!vars_is_name_valid(name) || vars_should_skip_import(name)) {
            arena_maybe_free(name);
            continue;
        }
        (void)vars_store(state, name, eq + 1, false, true, false);
        arena_maybe_free(name);
    }

    /*
     * POSIX shells initialize IFS to <space><tab><newline> regardless of
     * inherited environment so field splitting has a predictable baseline.
     */
    (void)vars_set_with_mode(state, "IFS", " \t\n", false, false);
    /* OPTIND starts at 1 for getopts parsing state in each fresh shell. */
    (void)vars_set_with_mode(state, "OPTIND", "1", false, false);
    (void)vars_set_with_mode(state, "LINENO", "1", false, false);
    (void)vars_unset(state, "OPTARG");
}

void vars_destroy(struct shell_state *state) {
    size_t i;

    for (i = 0; i < state->var_count; i++) {
        arena_maybe_free(state->vars[i].name);
        arena_maybe_free(state->vars[i].value);
    }
    arena_maybe_free(state->vars);
    state->vars = NULL;
    state->var_count = 0;
    vars_cache_invalidate(state);
}

bool vars_is_name_valid(const char *name) {
    size_t i;

    if (name == NULL || name[0] == '\0') {
        return false;
    }
    if (!(isalpha((unsigned char)name[0]) || name[0] == '_')) {
        return false;
    }
    for (i = 1; name[i] != '\0'; i++) {
        if (!(isalnum((unsigned char)name[i]) || name[i] == '_')) {
            return false;
        }
    }
    return true;
}

bool vars_is_readonly(const struct shell_state *state, const char *name) {
    const struct shell_var *var;

    var = vars_lookup((struct shell_state *)state, name);
    return var != NULL && var->readonly;
}

bool vars_is_set(const struct shell_state *state, const char *name) {
    return vars_lookup((struct shell_state *)state, name) != NULL;
}

bool vars_is_exported(const struct shell_state *state, const char *name) {
    const struct shell_var *var;

    var = vars_lookup((struct shell_state *)state, name);
    return var != NULL && var->exported;
}

bool vars_is_unexported(const struct shell_state *state, const char *name) {
    const struct shell_var *var;

    var = vars_lookup((struct shell_state *)state, name);
    return var != NULL && !var->exported;
}

const char *vars_get(const struct shell_state *state, const char *name) {
    return vars_get_n((struct shell_state *)state, name, strlen(name));
}

const char *vars_get_n(struct shell_state *state, const char *name, size_t len) {
    const struct shell_var *var;

    var = vars_lookup_n(state, name, len);
    if (var == NULL) {
        return NULL;
    }
    return var->value;
}

bool vars_get_long_n(struct shell_state *state, const char *name, size_t len,
                     long *out) {
    struct shell_var *var;
    long parsed;

    var = vars_lookup_mut_n(state, name, len);
    if (var == NULL || var->value_len == 0) {
        return false;
    }
    if (var->long_cache_valid) {
        *out = var->long_cache;
        return true;
    }
    if (!vars_parse_long_value(var->value, &parsed)) {
        *out = 0;
        return true;
    }

    var->long_cache_valid = true;
    var->long_cache = parsed;
    *out = parsed;
    return true;
}

int vars_set(struct shell_state *state, const char *name, const char *value,
             bool check_readonly) {
    return vars_set_with_mode(state, name, value, check_readonly, true);
}

int vars_set_with_mode(struct shell_state *state, const char *name,
                       const char *value, bool check_readonly, bool exported) {
    if (!vars_is_storage_name(name)) {
        posish_errorf("invalid variable name: %s", name);
        return 1;
    }
    return vars_store(state, name, value, check_readonly, exported, false);
}

int vars_set_assignment(struct shell_state *state, const char *name,
                        const char *value, bool check_readonly) {
    return vars_set_assignment_n(state, name, strlen(name), value,
                                 check_readonly);
}

int vars_set_assignment_n(struct shell_state *state, const char *name,
                          size_t len, const char *value, bool check_readonly) {
    bool exported;
    const struct shell_var *existing;

    if (!vars_is_name_valid_n(name, len)) {
        char *name_copy;

        name_copy = arena_alloc_in(NULL, len + 1);
        memcpy(name_copy, name, len);
        name_copy[len] = '\0';
        posish_errorf("invalid variable name: %s", name_copy);
        arena_maybe_free(name_copy);
        return 1;
    }

    existing = vars_lookup_n(state, name, len);
    exported = existing != NULL ? existing->exported : false;
    if (len == 4 && memcmp(name, "PATH", 4) == 0) {
        exported = true;
    }
    if (state->allexport) {
        exported = true;
    }

    return vars_store_n(state, name, len, value, check_readonly, exported, false);
}

int vars_set_assignment_long_n(struct shell_state *state, const char *name,
                               size_t len, long value,
                               bool check_readonly) {
    bool exported;
    struct shell_var *var;
    char *name_copy;

    if (!vars_is_name_valid_n(name, len)) {
        name_copy = arena_alloc_in(NULL, len + 1);
        memcpy(name_copy, name, len);
        name_copy[len] = '\0';
        posish_errorf("invalid variable name: %s", name_copy);
        arena_maybe_free(name_copy);
        return 1;
    }

    var = vars_lookup_mut_n(state, name, len);
    exported = var != NULL ? var->exported : false;
    if (len == 4 && memcmp(name, "PATH", 4) == 0) {
        exported = true;
    }
    if (state->allexport) {
        exported = true;
    }

    if (var != NULL) {
        if (check_readonly && var->readonly) {
            name_copy = arena_alloc_in(NULL, len + 1);
            memcpy(name_copy, name, len);
            name_copy[len] = '\0';
            posish_errorf("%s: is read-only", name_copy);
            arena_maybe_free(name_copy);
            return 1;
        }
        vars_replace_value_long(var, value);
        var->exported = exported;
        vars_cache_store(state, (size_t)(var - state->vars));
        vars_maybe_invalidate_path_cache_n(state, name, len);
        return 0;
    }

    {
        char buf[3 + sizeof(long) * 3];

        (void)vars_format_long_decimal(value, buf);
        return vars_append_new_n(state, name, len, buf, exported, false);
    }
}

int vars_mark_exported(struct shell_state *state, const char *name) {
    struct shell_var *var;

    if (!vars_is_name_valid(name)) {
        posish_errorf("export: invalid variable name: %s", name);
        return 1;
    }

    var = vars_lookup_mut(state, name);
    if (var == NULL) {
        return vars_append_new(state, name, "", true, false);
    }
    var->exported = true;
    vars_maybe_invalidate_path_cache(state, name);
    return 0;
}

int vars_unset(struct shell_state *state, const char *name) {
    ssize_t idx;
    size_t i;

    if (!vars_is_name_valid(name)) {
        posish_errorf("unset: invalid variable name: %s", name);
        return 1;
    }

    idx = vars_find(state, name);
    if (idx < 0) {
        return 0;
    }
    if (state->vars[idx].readonly) {
        posish_errorf("unset: %s: is read-only", name);
        return 1;
    }

    arena_maybe_free(state->vars[idx].name);
    arena_maybe_free(state->vars[idx].value);
    for (i = (size_t)idx + 1; i < state->var_count; i++) {
        state->vars[i - 1] = state->vars[i];
    }
    state->var_count--;
    vars_cache_invalidate(state);
    vars_maybe_invalidate_path_cache(state, name);
    return 0;
}

int vars_mark_readonly(struct shell_state *state, const char *name,
                       const char *value, bool with_value) {
    const struct shell_var *existing;
    bool exported;
    int rc;

    if (!vars_is_name_valid(name)) {
        posish_errorf("readonly: invalid variable name: %s", name);
        return 1;
    }

    existing = vars_lookup(state, name);
    exported = existing != NULL ? existing->exported : false;
    if (with_value) {
        rc = vars_store(state, name, value, true, exported, true);
        if (rc != 0) {
            return rc;
        }
        return 0;
    }

    return vars_mark_readonly_existing(state, name);
}

char **vars_build_envp(const struct shell_state *state, size_t *count_out) {
    char **envp;
    size_t count;
    size_t i;

    count = 0;
    for (i = 0; i < state->var_count; i++) {
        if (state->vars[i].exported) {
            count++;
        }
    }

    envp = arena_alloc_in(NULL, sizeof(*envp) * (count + 1));
    count = 0;
    for (i = 0; i < state->var_count; i++) {
        size_t nlen;
        size_t vlen;
        char *entry;

        if (!state->vars[i].exported) {
            continue;
        }

        nlen = strlen(state->vars[i].name);
        vlen = strlen(state->vars[i].value);
        entry = arena_alloc_in(NULL, nlen + 1 + vlen + 1);
        memcpy(entry, state->vars[i].name, nlen);
        entry[nlen] = '=';
        memcpy(entry + nlen + 1, state->vars[i].value, vlen + 1);
        envp[count++] = entry;
    }
    envp[count] = NULL;
    if (count_out != NULL) {
        *count_out = count;
    }
    return envp;
}

char **vars_build_exec_envp(const struct shell_state *state) {
    static const char key[] = "POSISH_PARENT_INTERACTIVE=";
    char **envp;
    size_t count;
    size_t i;

    envp = vars_build_envp(state, &count);
    for (i = 0; i < count; i++) {
        if (strncmp(envp[i], key, sizeof(key) - 1) == 0) {
            arena_maybe_free(envp[i]);
            envp[i] =
                arena_alloc_in(NULL, sizeof(key) + 1);
            memcpy(envp[i], key, sizeof(key) - 1);
            envp[i][sizeof(key) - 1] = state->interactive ? '1' : '0';
            envp[i][sizeof(key)] = '\0';
            return envp;
        }
    }

    envp = arena_realloc_in(NULL, envp, sizeof(*envp) * (count + 2));
    envp[count] = arena_alloc_in(NULL, sizeof(key) + 1);
    memcpy(envp[count], key, sizeof(key) - 1);
    envp[count][sizeof(key) - 1] = state->interactive ? '1' : '0';
    envp[count][sizeof(key)] = '\0';
    envp[count + 1] = NULL;
    return envp;
}

void vars_free_envp(char **envp) {
    size_t i;

    if (envp == NULL) {
        return;
    }
    for (i = 0; envp[i] != NULL; i++) {
        arena_maybe_free(envp[i]);
    }
    arena_maybe_free(envp);
}
