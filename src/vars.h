/* SPDX-License-Identifier: 0BSD */

/* posish - variable interface */

#ifndef POSISH_VARS_H
#define POSISH_VARS_H

#include <stdbool.h>

#include "shell.h"

void vars_init(void);
bool vars_is_name_valid(const char *name);
bool vars_is_readonly(const struct shell_state *state, const char *name);
bool vars_is_unexported(const struct shell_state *state, const char *name);
int vars_set(struct shell_state *state, const char *name, const char *value,
             bool check_readonly);
int vars_set_with_mode(struct shell_state *state, const char *name,
                       const char *value, bool check_readonly, bool exported);
int vars_set_assignment(struct shell_state *state, const char *name,
                        const char *value, bool check_readonly);
int vars_mark_exported(struct shell_state *state, const char *name);
int vars_unset(struct shell_state *state, const char *name);
int vars_mark_readonly(struct shell_state *state, const char *name,
                       const char *value, bool with_value);
void vars_apply_unexported_in_child(const struct shell_state *state);

#endif
