/* SPDX-License-Identifier: 0BSD */

/* posish - signal interface */

#ifndef POSISH_SIGNALS_H
#define POSISH_SIGNALS_H

#include <stdbool.h>

void signals_init(void);
void signals_apply_policy(bool interactive, bool monitor_mode);
bool signals_inherited_ignored(int signo);
bool signals_policy_ignored(int signo);
int signals_set_default(int signo);
int signals_set_ignored(int signo);
int signals_set_trap(int signo);
int signals_take_next_pending(void);
void signals_clear_pending(int signo);

#endif
