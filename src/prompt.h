/* SPDX-License-Identifier: 0BSD */

/* posish - prompt interface */

#ifndef POSISH_PROMPT_H
#define POSISH_PROMPT_H

struct shell_state;

int prompt_render(struct shell_state *state, const char *var_name, char **out);
void prompt_init_defaults(struct shell_state *state, const char *argv0);

#endif
