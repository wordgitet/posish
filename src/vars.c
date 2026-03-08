/* SPDX-License-Identifier: 0BSD */

/* posish - variable storage */

#include "vars.h"

#include "arena.h"
#include "error.h"
#include "path.h"
#include "symbols.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

extern char **environ;

struct shell_var_entry {
    struct symbol_node sym;
    struct shell_var var;
};

static struct shell_var_entry *vars_entry_from_node(struct symbol_node *node) {
    return (struct shell_var_entry *)((char *)node -
                                      offsetof(struct shell_var_entry, sym));
}

static const struct shell_var_entry *
vars_entry_from_const_node(const struct symbol_node *node) {
    return (const struct shell_var_entry *)((const char *)node -
                                            offsetof(struct shell_var_entry, sym));
}

static bool vars_is_process_marker(const char *name) {
    return strcmp(name, "POSISH_PARENT_INTERACTIVE") == 0 ||
           strcmp(name, "POSISH_TRACE") == 0 ||
           strcmp(name, "POSISH_LINENO_BASE") == 0;
}

static bool vars_should_skip_import(const char *name) {
    return vars_is_process_marker(name);
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

static void vars_cache_store(struct shell_state *state,
                             struct shell_var_entry *entry) {
    state->var_mru = entry;
}

static void vars_cache_invalidate(struct shell_state *state) {
    state->var_mru = NULL;
}

static bool vars_cache_matches(const struct shell_var_entry *entry,
                               const char *name, size_t len) {
    size_t stored_len;

    if (entry == NULL) {
        return false;
    }
    stored_len = strlen(entry->sym.name);
    return stored_len == len && entry->sym.name[0] == name[0] &&
           memcmp(entry->sym.name, name, len) == 0;
}

static struct shell_var_entry *vars_lookup_entry_n(struct shell_state *state,
                                                   const char *name,
                                                   size_t len) {
    struct symbol_node *node;

    if (vars_cache_matches(state->var_mru, name, len)) {
        return state->var_mru;
    }

    node = symbol_table_lookup_n(&state->vars_table, name, len);
    if (node == NULL) {
        return NULL;
    }

    vars_cache_store(state, vars_entry_from_node(node));
    return vars_entry_from_node(node);
}

static struct shell_var_entry *vars_lookup_entry(struct shell_state *state,
                                                 const char *name) {
    return vars_lookup_entry_n(state, name, strlen(name));
}

static const struct shell_var_entry *
vars_lookup_const_entry_n(struct shell_state *state, const char *name,
                          size_t len) {
    return vars_lookup_entry_n(state, name, len);
}

static const struct shell_var_entry *
vars_lookup_const_entry(struct shell_state *state, const char *name) {
    return vars_lookup_entry(state, name);
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

static size_t vars_format_long_decimal(long value,
                                       char buf[static 3 + sizeof(long) * 3]) {
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

static void vars_destroy_entry(struct shell_var_entry *entry) {
    free(entry->sym.name);
    free(entry->var.value);
    free(entry);
}

static void vars_destroy_node(struct symbol_node *node) {
    vars_destroy_entry(vars_entry_from_node(node));
}

static struct shell_var_entry *vars_alloc_entry_n(const char *name, size_t len,
                                                  const char *value,
                                                  bool exported,
                                                  bool readonly) {
    struct shell_var_entry *entry;
    const char *stored_value;
    long parsed_long;

    entry = arena_alloc_in(NULL, sizeof(*entry));
    memset(entry, 0, sizeof(*entry));

    entry->sym.name = arena_alloc_in(NULL, len + 1);
    memcpy(entry->sym.name, name, len);
    entry->sym.name[len] = '\0';
    entry->sym.hash = symbol_hash_n(name, len);

    stored_value = value == NULL ? "" : value;
    entry->var.value = arena_strdup_in(NULL, stored_value);
    entry->var.value_len = strlen(stored_value);
    parsed_long = 0;
    entry->var.long_cache_valid =
        vars_parse_long_value(stored_value, &parsed_long);
    entry->var.long_cache = parsed_long;
    entry->var.exported = exported;
    entry->var.readonly = readonly;
    return entry;
}

static int vars_append_new_n(struct shell_state *state, const char *name,
                             size_t len, const char *value, bool exported,
                             bool readonly) {
    struct shell_var_entry *entry;
    int rc;

    entry = vars_alloc_entry_n(name, len, value, exported, readonly);
    rc = symbol_table_insert(&state->vars_table, &entry->sym);
    if (rc != 0) {
        vars_destroy_entry(entry);
        return rc;
    }

    vars_cache_store(state, entry);
    vars_maybe_invalidate_path_cache_n(state, name, len);
    return 0;
}

static int vars_append_new(struct shell_state *state, const char *name,
                           const char *value, bool exported, bool readonly) {
    return vars_append_new_n(state, name, strlen(name), value, exported,
                             readonly);
}

static void vars_replace_value(struct shell_var *var, const char *value) {
    size_t len;
    long parsed_long;
    const char *stored_value;

    stored_value = value == NULL ? "" : value;
    len = strlen(stored_value);
    var->value = arena_realloc_in(NULL, var->value, len + 1);
    memcpy(var->value, stored_value, len + 1);
    var->value_len = len;
    parsed_long = 0;
    var->long_cache_valid = vars_parse_long_value(stored_value, &parsed_long);
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

static int vars_store_n(struct shell_state *state, const char *name, size_t len,
                        const char *value, bool check_readonly, bool exported,
                        bool mark_readonly) {
    struct shell_var_entry *entry;
    char *name_copy;

    entry = vars_lookup_entry_n(state, name, len);
    if (entry != NULL) {
        if (check_readonly && entry->var.readonly) {
            name_copy = arena_alloc_in(NULL, len + 1);
            memcpy(name_copy, name, len);
            name_copy[len] = '\0';
            posish_errorf("%s: is read-only", name_copy);
            free(name_copy);
            return 1;
        }
        vars_replace_value(&entry->var, value);
        entry->var.exported = exported;
        if (mark_readonly) {
            entry->var.readonly = true;
        }
        vars_cache_store(state, entry);
        vars_maybe_invalidate_path_cache_n(state, name, len);
        return 0;
    }

    return vars_append_new_n(state, name, len, value, exported, mark_readonly);
}

static int vars_store(struct shell_state *state, const char *name,
                      const char *value, bool check_readonly, bool exported,
                      bool mark_readonly) {
    return vars_store_n(state, name, strlen(name), value, check_readonly,
                        exported, mark_readonly);
}

static int vars_mark_readonly_existing(struct shell_state *state,
                                       const char *name) {
    struct shell_var_entry *entry;

    entry = vars_lookup_entry(state, name);
    if (entry == NULL) {
        return vars_append_new(state, name, "", false, true);
    }
    entry->var.readonly = true;
    vars_cache_store(state, entry);
    return 0;
}

void vars_init(struct shell_state *state) {
    size_t i;

    symbol_table_init(&state->vars_table);
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
            free(name);
            continue;
        }
        (void)vars_store(state, name, eq + 1, false, true, false);
        free(name);
    }

    (void)vars_set_with_mode(state, "IFS", " \t\n", false, false);
    (void)vars_set_with_mode(state, "OPTIND", "1", false, false);
    (void)vars_set_with_mode(state, "LINENO", "1", false, false);
    (void)vars_unset(state, "OPTARG");
}

void vars_destroy(struct shell_state *state) {
    symbol_table_destroy(&state->vars_table, vars_destroy_node);
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
    const struct shell_var_entry *entry;

    entry = vars_lookup_const_entry((struct shell_state *)state, name);
    return entry != NULL && entry->var.readonly;
}

bool vars_is_set(const struct shell_state *state, const char *name) {
    return vars_lookup_const_entry((struct shell_state *)state, name) != NULL;
}

bool vars_is_exported(const struct shell_state *state, const char *name) {
    const struct shell_var_entry *entry;

    entry = vars_lookup_const_entry((struct shell_state *)state, name);
    return entry != NULL && entry->var.exported;
}

bool vars_is_unexported(const struct shell_state *state, const char *name) {
    const struct shell_var_entry *entry;

    entry = vars_lookup_const_entry((struct shell_state *)state, name);
    return entry != NULL && !entry->var.exported;
}

const char *vars_get(const struct shell_state *state, const char *name) {
    return vars_get_n((struct shell_state *)state, name, strlen(name));
}

const char *vars_get_n(struct shell_state *state, const char *name, size_t len) {
    const struct shell_var_entry *entry;

    entry = vars_lookup_const_entry_n(state, name, len);
    if (entry == NULL) {
        return NULL;
    }
    return entry->var.value;
}

bool vars_get_long_n(struct shell_state *state, const char *name, size_t len,
                     long *out) {
    struct shell_var_entry *entry;
    long parsed;

    entry = vars_lookup_entry_n(state, name, len);
    if (entry == NULL || entry->var.value_len == 0) {
        return false;
    }
    if (entry->var.long_cache_valid) {
        *out = entry->var.long_cache;
        return true;
    }
    if (!vars_parse_long_value(entry->var.value, &parsed)) {
        *out = 0;
        return true;
    }

    entry->var.long_cache_valid = true;
    entry->var.long_cache = parsed;
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
    const struct shell_var_entry *existing;

    if (!vars_is_name_valid_n(name, len)) {
        char *name_copy;

        name_copy = arena_alloc_in(NULL, len + 1);
        memcpy(name_copy, name, len);
        name_copy[len] = '\0';
        posish_errorf("invalid variable name: %s", name_copy);
        free(name_copy);
        return 1;
    }

    existing = vars_lookup_const_entry_n(state, name, len);
    exported = existing != NULL ? existing->var.exported : false;
    if (len == 4 && memcmp(name, "PATH", 4) == 0) {
        exported = true;
    }
    if (state->allexport) {
        exported = true;
    }

    return vars_store_n(state, name, len, value, check_readonly, exported,
                        false);
}

int vars_set_assignment_long_n(struct shell_state *state, const char *name,
                               size_t len, long value,
                               bool check_readonly) {
    bool exported;
    struct shell_var_entry *entry;
    char *name_copy;

    if (!vars_is_name_valid_n(name, len)) {
        name_copy = arena_alloc_in(NULL, len + 1);
        memcpy(name_copy, name, len);
        name_copy[len] = '\0';
        posish_errorf("invalid variable name: %s", name_copy);
        free(name_copy);
        return 1;
    }

    entry = vars_lookup_entry_n(state, name, len);
    exported = entry != NULL ? entry->var.exported : false;
    if (len == 4 && memcmp(name, "PATH", 4) == 0) {
        exported = true;
    }
    if (state->allexport) {
        exported = true;
    }

    if (entry != NULL) {
        if (check_readonly && entry->var.readonly) {
            name_copy = arena_alloc_in(NULL, len + 1);
            memcpy(name_copy, name, len);
            name_copy[len] = '\0';
            posish_errorf("%s: is read-only", name_copy);
            free(name_copy);
            return 1;
        }
        vars_replace_value_long(&entry->var, value);
        entry->var.exported = exported;
        vars_cache_store(state, entry);
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
    struct shell_var_entry *entry;

    if (!vars_is_name_valid(name)) {
        posish_errorf("export: invalid variable name: %s", name);
        return 1;
    }

    entry = vars_lookup_entry(state, name);
    if (entry == NULL) {
        return vars_append_new(state, name, "", true, false);
    }
    entry->var.exported = true;
    vars_cache_store(state, entry);
    vars_maybe_invalidate_path_cache(state, name);
    return 0;
}

int vars_unset(struct shell_state *state, const char *name) {
    struct symbol_node *node;
    struct shell_var_entry *entry;

    if (!vars_is_name_valid(name)) {
        posish_errorf("unset: invalid variable name: %s", name);
        return 1;
    }

    entry = vars_lookup_entry(state, name);
    if (entry == NULL) {
        return 0;
    }
    if (entry->var.readonly) {
        posish_errorf("unset: %s: is read-only", name);
        return 1;
    }

    node = symbol_table_remove(&state->vars_table, name);
    if (node != NULL) {
        vars_destroy_node(node);
    }
    vars_cache_invalidate(state);
    vars_maybe_invalidate_path_cache(state, name);
    return 0;
}

int vars_mark_readonly(struct shell_state *state, const char *name,
                       const char *value, bool with_value) {
    const struct shell_var_entry *existing;
    bool exported;
    int rc;

    if (!vars_is_name_valid(name)) {
        posish_errorf("readonly: invalid variable name: %s", name);
        return 1;
    }

    existing = vars_lookup_const_entry(state, name);
    exported = existing != NULL ? existing->var.exported : false;
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
    const struct symbol_node *node;

    count = 0;
    for (node = symbol_table_first(&state->vars_table); node != NULL;
         node = symbol_table_next(node)) {
        const struct shell_var_entry *entry;

        entry = vars_entry_from_const_node(node);
        if (entry->var.exported) {
            count++;
        }
    }

    envp = arena_alloc_in(NULL, sizeof(*envp) * (count + 1));
    i = 0;
    for (node = symbol_table_first(&state->vars_table); node != NULL;
         node = symbol_table_next(node)) {
        const struct shell_var_entry *entry;
        size_t nlen;
        size_t vlen;
        char *item;

        entry = vars_entry_from_const_node(node);
        if (!entry->var.exported) {
            continue;
        }

        nlen = strlen(entry->sym.name);
        vlen = entry->var.value_len;
        item = arena_alloc_in(NULL, nlen + 1 + vlen + 1);
        memcpy(item, entry->sym.name, nlen);
        item[nlen] = '=';
        memcpy(item + nlen + 1, entry->var.value, vlen + 1);
        envp[i++] = item;
    }
    envp[i] = NULL;
    if (count_out != NULL) {
        *count_out = i;
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
            free(envp[i]);
            envp[i] = arena_alloc_in(NULL, sizeof(key) + 1);
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
        free(envp[i]);
    }
    free(envp);
}

void vars_for_each(const struct shell_state *state, vars_visit_fn visit,
                   void *user_data) {
    const struct symbol_node *node;

    for (node = symbol_table_first(&state->vars_table); node != NULL;
         node = symbol_table_next(node)) {
        const struct shell_var_entry *entry;

        entry = vars_entry_from_const_node(node);
        if (!visit(entry->sym.name, &entry->var, user_data)) {
            return;
        }
    }
}
