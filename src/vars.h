/* SPDX-License-Identifier: 0BSD */

/* posish - variable interface */

#ifndef POSISH_VARS_H
#define POSISH_VARS_H

#include <stdbool.h>
#include <stddef.h>

#include "shell.h"

typedef bool (*vars_visit_fn)(const char *name, const struct shell_var *var,
                              void *user_data);

void vars_init(struct shell_state *state);
void vars_destroy(struct shell_state *state);
bool vars_is_name_valid(const char *name);
bool vars_is_readonly(const struct shell_state *state, const char *name);
bool vars_is_set(const struct shell_state *state, const char *name);
bool vars_is_exported(const struct shell_state *state, const char *name);
bool vars_is_unexported(const struct shell_state *state, const char *name);
const char *vars_get(const struct shell_state *state, const char *name);
const char *vars_get_n(struct shell_state *state, const char *name, size_t len);
bool vars_get_long_n(struct shell_state *state, const char *name, size_t len,
                     long *out);
int vars_set(struct shell_state *state, const char *name, const char *value,
             bool check_readonly);
int vars_set_with_mode(struct shell_state *state, const char *name,
                       const char *value, bool check_readonly, bool exported);
int vars_set_assignment(struct shell_state *state, const char *name,
                        const char *value, bool check_readonly);
int vars_set_assignment_n(struct shell_state *state, const char *name,
                          size_t len, const char *value, bool check_readonly);
int vars_set_assignment_long_n(struct shell_state *state, const char *name,
                               size_t len, long value,
                               bool check_readonly);
int vars_mark_exported(struct shell_state *state, const char *name);
int vars_unset(struct shell_state *state, const char *name);
int vars_mark_readonly(struct shell_state *state, const char *name,
                       const char *value, bool with_value);
char **vars_build_envp(struct shell_state *state, size_t *count_out);
char **vars_build_exec_envp(struct shell_state *state);
void vars_free_envp(struct shell_state *state, char **envp);
void vars_for_each(const struct shell_state *state, vars_visit_fn visit,
                   void *user_data);

#endif
