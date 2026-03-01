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
