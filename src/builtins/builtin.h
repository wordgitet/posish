/* SPDX-License-Identifier: 0BSD */

/* posish - builtin dispatch interface */

#ifndef POSISH_BUILTIN_H
#define POSISH_BUILTIN_H

#include <stdbool.h>

#include "shell.h"

int builtin_try_special(struct shell_state *state, char *const argv[], bool *handled);
int builtin_dispatch(struct shell_state *state, char *const argv[], bool *handled);
bool builtin_is_special_name(const char *name);
bool builtin_is_name(const char *name);

#endif
