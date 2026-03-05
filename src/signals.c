/* SPDX-License-Identifier: 0BSD */

/* posish - signal policy and traps */

#include "signals.h"
#include "trace.h"

#include <signal.h>
#include <stddef.h>

static bool g_inherited_ignored[NSIG];
static bool g_policy_ignored[NSIG];
static volatile sig_atomic_t g_pending[NSIG];

static void trap_handler(int signo) {
    if (signo > 0 && signo < NSIG) {
        g_pending[signo] = 1;
    }
}

static int set_signal_action(int signo, void (*handler)(int)) {
    struct sigaction sa;

    sa.sa_handler = handler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    return sigaction(signo, &sa, NULL);
}

void signals_init(void) {
    int signo;

    /*
     * Track startup dispositions so `trap -` can restore inherited behavior
     * rather than always forcing SIG_DFL.
     */
    for (signo = 0; signo < NSIG; signo++) {
        struct sigaction sa;

        g_inherited_ignored[signo] = false;
        g_policy_ignored[signo] = false;
        if (signo == 0) {
            continue;
        }
        g_pending[signo] = 0;
        if (sigaction(signo, NULL, &sa) == 0 && sa.sa_handler == SIG_IGN) {
            g_inherited_ignored[signo] = true;
        }
    }
}

void signals_apply_policy(bool interactive, bool monitor_mode) {
    int signo;

    trace_log(POSISH_TRACE_SIGNALS,
              "apply policy interactive=%d monitor_mode=%d",
              interactive ? 1 : 0, monitor_mode ? 1 : 0);

    for (signo = 0; signo < NSIG; signo++) {
        g_policy_ignored[signo] = false;
    }

    if (interactive) {
#ifdef SIGINT
        if (signals_set_ignored(SIGINT) == 0) {
            g_policy_ignored[SIGINT] = true;
        }
#endif
#ifdef SIGQUIT
        if (signals_set_ignored(SIGQUIT) == 0) {
            g_policy_ignored[SIGQUIT] = true;
        }
#endif
#ifdef SIGTERM
        if (signals_set_ignored(SIGTERM) == 0) {
            g_policy_ignored[SIGTERM] = true;
        }
#endif
    }

    if (monitor_mode) {
#ifdef SIGTSTP
        if (signals_set_ignored(SIGTSTP) == 0) {
            g_policy_ignored[SIGTSTP] = true;
        }
#endif
#ifdef SIGTTIN
        if (signals_set_ignored(SIGTTIN) == 0) {
            g_policy_ignored[SIGTTIN] = true;
        }
#endif
#ifdef SIGTTOU
        if (signals_set_ignored(SIGTTOU) == 0) {
            g_policy_ignored[SIGTTOU] = true;
        }
#endif
    }
}

bool signals_inherited_ignored(int signo) {
    if (signo <= 0 || signo >= NSIG) {
        return false;
    }
    return g_inherited_ignored[signo];
}

bool signals_policy_ignored(int signo) {
    if (signo <= 0 || signo >= NSIG) {
        return false;
    }
    return g_policy_ignored[signo];
}

int signals_set_default(int signo) {
    int rc;

    if (signo <= 0 || signo >= NSIG) {
        return -1;
    }
    g_pending[signo] = 0;
    rc = set_signal_action(signo, SIG_DFL);
    trace_log(POSISH_TRACE_SIGNALS, "set default signal=%d rc=%d", signo, rc);
    return rc;
}

int signals_set_ignored(int signo) {
    int rc;

    if (signo <= 0 || signo >= NSIG) {
        return -1;
    }
    g_pending[signo] = 0;
    rc = set_signal_action(signo, SIG_IGN);
    trace_log(POSISH_TRACE_SIGNALS, "set ignore signal=%d rc=%d", signo, rc);
    return rc;
}

int signals_set_trap(int signo) {
    int rc;

    if (signo <= 0 || signo >= NSIG) {
        return -1;
    }
    g_pending[signo] = 0;
    rc = set_signal_action(signo, trap_handler);
    trace_log(POSISH_TRACE_SIGNALS, "set trap handler signal=%d rc=%d", signo,
              rc);
    return rc;
}

int signals_take_next_pending(void) {
    int signo;

    for (signo = 1; signo < NSIG; signo++) {
        if (g_pending[signo] != 0) {
            g_pending[signo] = 0;
            trace_log(POSISH_TRACE_SIGNALS, "take pending signal=%d", signo);
            return signo;
        }
    }
    return 0;
}

void signals_clear_pending(int signo) {
    if (signo > 0 && signo < NSIG) {
        g_pending[signo] = 0;
        trace_log(POSISH_TRACE_SIGNALS, "clear pending signal=%d", signo);
    }
}

/* ---------- child trap / signal disposition (moved from exec.c) ---------- */

#include "shell.h"

static bool inherited_ignore_locked(const struct shell_state *state, int signo) {
    return !state->interactive && signals_inherited_ignored(signo) &&
           !state->parent_was_interactive;
}

static bool trap_clear_keeps_ignore(const struct shell_state *state, int signo) {
    return inherited_ignore_locked(state, signo);
}

static bool trace_signal_of_interest(int signo) {
#ifdef SIGINT
    if (signo == SIGINT) return true;
#endif
#ifdef SIGQUIT
    if (signo == SIGQUIT) return true;
#endif
#ifdef SIGTERM
    if (signo == SIGTERM) return true;
#endif
#ifdef SIGTSTP
    if (signo == SIGTSTP) return true;
#endif
#ifdef SIGTTIN
    if (signo == SIGTTIN) return true;
#endif
#ifdef SIGTTOU
    if (signo == SIGTTOU) return true;
#endif
    return false;
}

static void apply_child_signal_disposition(const struct shell_state *state,
                                           char **trap_slot, int signo,
                                           bool reset_policy_ignore) {
    const char *trap_value;

    trap_value = trap_slot != NULL ? *trap_slot : state->signal_traps[signo];
    if (trap_value != NULL) {
        if (trap_value[0] == '\0') {
            (void)signals_set_ignored(signo);
        } else if (inherited_ignore_locked(state, signo)) {
            if (trap_slot != NULL) *trap_slot = NULL;
            (void)signals_set_ignored(signo);
        } else {
            if (trap_slot != NULL) *trap_slot = NULL;
            (void)signals_set_default(signo);
        }
        signals_clear_pending(signo);
        return;
    }

    if (state->signal_cleared[signo]) {
        if (trap_clear_keeps_ignore(state, signo)) {
            (void)signals_set_ignored(signo);
        } else {
            (void)signals_set_default(signo);
        }
        signals_clear_pending(signo);
        return;
    }

    if (reset_policy_ignore && signals_policy_ignored(signo) &&
        !inherited_ignore_locked(state, signo) &&
        !(state->interactive && signals_inherited_ignored(signo))) {
        (void)signals_set_default(signo);
    }
    signals_clear_pending(signo);
}

void signals_reset_traps_for_child(struct shell_state *state) {
    int signo;

    trace_log(POSISH_TRACE_SIGNALS,
              "reset child traps interactive=%d main=%d async=%d monitor=%d",
              state->interactive ? 1 : 0, state->main_context ? 1 : 0,
              state->in_async_context ? 1 : 0, state->monitor_mode ? 1 : 0);

    for (signo = 1; signo < NSIG; signo++) {
        if (trace_signal_of_interest(signo)) {
            trace_log(POSISH_TRACE_SIGNALS,
                      "child reset signo=%d trap=%s cleared=%d inherited_ign=%d policy_ign=%d",
                      signo,
                      state->signal_traps[signo] == NULL ? "(null)"
                          : (state->signal_traps[signo][0] == '\0' ? "ignore" : "command"),
                      state->signal_cleared[signo] ? 1 : 0,
                      signals_inherited_ignored(signo) ? 1 : 0,
                      signals_policy_ignored(signo) ? 1 : 0);
        }
        apply_child_signal_disposition(state, &state->signal_traps[signo], signo, true);
    }
}

void signals_reset_exit_trap_for_child(struct shell_state *state) {
    state->exit_trap = NULL;
}

void exec_prepare_signals_for_exec_child(const struct shell_state *state) {
    int signo;

    trace_log(POSISH_TRACE_SIGNALS,
              "prepare exec child interactive=%d main=%d async=%d monitor=%d",
              state->interactive ? 1 : 0, state->main_context ? 1 : 0,
              state->in_async_context ? 1 : 0, state->monitor_mode ? 1 : 0);

    for (signo = 1; signo < NSIG; signo++) {
        if (trace_signal_of_interest(signo)) {
            trace_log(POSISH_TRACE_SIGNALS,
                      "exec child signo=%d trap=%s cleared=%d inherited_ign=%d policy_ign=%d",
                      signo,
                      state->signal_traps[signo] == NULL ? "(null)"
                          : (state->signal_traps[signo][0] == '\0' ? "ignore" : "command"),
                      state->signal_cleared[signo] ? 1 : 0,
                      signals_inherited_ignored(signo) ? 1 : 0,
                      signals_policy_ignored(signo) ? 1 : 0);
        }

#ifdef SIGINT
        if (signo == SIGINT && state->in_async_context && !state->monitor_mode) {
            if (state->signal_traps[signo] == NULL && !state->signal_cleared[signo]) {
                (void)signals_set_ignored(signo);
                signals_clear_pending(signo);
                continue;
            }
            if (state->signal_traps[signo] != NULL &&
                state->signal_traps[signo][0] == '\0') {
                (void)signals_set_ignored(signo);
                signals_clear_pending(signo);
                continue;
            }
        }
#endif
#ifdef SIGQUIT
        if (signo == SIGQUIT && state->in_async_context && !state->monitor_mode) {
            if (state->signal_traps[signo] == NULL && !state->signal_cleared[signo]) {
                (void)signals_set_ignored(signo);
                signals_clear_pending(signo);
                continue;
            }
            if (state->signal_traps[signo] != NULL &&
                state->signal_traps[signo][0] == '\0') {
                (void)signals_set_ignored(signo);
                signals_clear_pending(signo);
                continue;
            }
        }
#endif
        apply_child_signal_disposition(state, NULL, signo, state->main_context);
    }
}
