/* SPDX-License-Identifier: 0BSD */

/* posish - arithmetic interface */

#ifndef POSISH_ARITH_H
#define POSISH_ARITH_H

#include "shell.h"

int arith_eval(const char *expr, struct shell_state *state, long *out_value);

#endif
