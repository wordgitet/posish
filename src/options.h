/* SPDX-License-Identifier: 0BSD */

/* posish - options interface */

#ifndef POSISH_OPTIONS_H
#define POSISH_OPTIONS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "shell.h"

void options_init(void);
bool options_apply_short(struct shell_state *state, char short_name, bool enable,
                         bool *refresh_signal_policy);
bool options_apply_long(struct shell_state *state, const char *long_name,
                        bool enable, bool *refresh_signal_policy);
void options_format_dollar_minus(const struct shell_state *state, char *out,
                                 size_t out_size);
int options_print_set_o(FILE *out, const struct shell_state *state);
int options_print_set_plus_o(FILE *out, const struct shell_state *state);

#endif
