/* SPDX-License-Identifier: 0BSD */

/* posish - variable storage */

#include "vars.h"

#include "arena.h"
#include "error.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

static ssize_t vars_find_unexported(const struct shell_state *state,
                                    const char *name);

void vars_init(void) {
    /* Placeholder for shell variable storage and scope management. */
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
    size_t i;

    for (i = 0; i < state->readonly_count; i++) {
        if (strcmp(state->readonly_names[i], name) == 0) {
            return true;
        }
    }
    return false;
}

bool vars_is_unexported(const struct shell_state *state, const char *name) {
    return vars_find_unexported(state, name) >= 0;
}

static ssize_t vars_find_unexported(const struct shell_state *state,
                                    const char *name) {
    size_t i;

    for (i = 0; i < state->unexported_count; i++) {
        if (strcmp(state->unexported_names[i], name) == 0) {
            return (ssize_t)i;
        }
    }
    return -1;
}

static int vars_add_unexported(struct shell_state *state, const char *name) {
    char **new_names;

    if (vars_find_unexported(state, name) >= 0) {
        return 0;
    }

    new_names = arena_xrealloc(state->unexported_names,
                               sizeof(*state->unexported_names) *
                                   (state->unexported_count + 1));
    state->unexported_names = new_names;
    state->unexported_names[state->unexported_count++] = arena_xstrdup(name);
    return 0;
}

static void vars_remove_unexported(struct shell_state *state, const char *name) {
    ssize_t idx;
    size_t i;

    idx = vars_find_unexported(state, name);
    if (idx < 0) {
        return;
    }

    free(state->unexported_names[idx]);
    for (i = (size_t)idx + 1; i < state->unexported_count; i++) {
        state->unexported_names[i - 1] = state->unexported_names[i];
    }
    state->unexported_count--;
}

static int vars_add_readonly(struct shell_state *state, const char *name) {
    char **new_names;

    if (vars_is_readonly(state, name)) {
        return 0;
    }

    new_names = arena_xrealloc(state->readonly_names,
                               sizeof(*state->readonly_names) *
                                   (state->readonly_count + 1));
    state->readonly_names = new_names;
    state->readonly_names[state->readonly_count++] = arena_xstrdup(name);
    return 0;
}

int vars_set(struct shell_state *state, const char *name, const char *value,
             bool check_readonly) {
    return vars_set_with_mode(state, name, value, check_readonly, true);
}

int vars_set_with_mode(struct shell_state *state, const char *name,
                       const char *value, bool check_readonly, bool exported) {
    if (!vars_is_name_valid(name)) {
        posish_errorf("invalid variable name: %s", name);
        return 1;
    }
    if (check_readonly && vars_is_readonly(state, name)) {
        posish_errorf("%s: is read-only", name);
        return 1;
    }
    if (setenv(name, value, 1) != 0) {
        perror("setenv");
        return 1;
    }

    if (exported) {
        vars_remove_unexported(state, name);
    } else {
        vars_add_unexported(state, name);
    }
    return 0;
}

int vars_set_assignment(struct shell_state *state, const char *name,
                        const char *value, bool check_readonly) {
    bool exported;
    const char *existing;

    exported = state->allexport;
    if (strcmp(name, "PATH") == 0) {
        exported = true;
    }
    existing = getenv(name);
    if (!exported && existing != NULL && !vars_is_unexported(state, name)) {
        /*
         * Assignment preserves existing export state unless `set -a` forces
         * export for all assignments.
         */
        exported = true;
    }

    return vars_set_with_mode(state, name, value, check_readonly, exported);
}

int vars_mark_exported(struct shell_state *state, const char *name) {
    const char *value;

    if (!vars_is_name_valid(name)) {
        posish_errorf("export: invalid variable name: %s", name);
        return 1;
    }

    value = getenv(name);
    if (value == NULL) {
        if (setenv(name, "", 1) != 0) {
            perror("setenv");
            return 1;
        }
    }

    vars_remove_unexported(state, name);
    return 0;
}

int vars_unset(struct shell_state *state, const char *name) {
    if (!vars_is_name_valid(name)) {
        posish_errorf("unset: invalid variable name: %s", name);
        return 1;
    }
    if (vars_is_readonly(state, name)) {
        posish_errorf("unset: %s: is read-only", name);
        return 1;
    }
    if (unsetenv(name) != 0) {
        perror("unsetenv");
        return 1;
    }
    vars_remove_unexported(state, name);
    return 0;
}

int vars_mark_readonly(struct shell_state *state, const char *name,
                       const char *value, bool with_value) {
    if (!vars_is_name_valid(name)) {
        posish_errorf("readonly: invalid variable name: %s", name);
        return 1;
    }
    if (with_value) {
        if (vars_set(state, name, value, true) != 0) {
            return 1;
        }
    }
    return vars_add_readonly(state, name);
}

void vars_apply_unexported_in_child(const struct shell_state *state) {
    size_t i;

    for (i = 0; i < state->unexported_count; i++) {
        (void)unsetenv(state->unexported_names[i]);
    }
}
