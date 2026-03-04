/* SPDX-License-Identifier: 0BSD */

/* posish - trace logging */

#include "trace.h"

#if defined(POSISH_ENABLE_TRACE) && POSISH_ENABLE_TRACE

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "arena.h"

static bool g_trace_initialized;
static unsigned int g_trace_mask;

static unsigned int trace_all_mask(void) {
    return POSISH_TRACE_SIGNALS | POSISH_TRACE_JOBS | POSISH_TRACE_TRAPS;
}

static const char *trace_category_name(unsigned int category) {
    if (category == POSISH_TRACE_SIGNALS) {
        return "signals";
    }
    if (category == POSISH_TRACE_JOBS) {
        return "jobs";
    }
    if (category == POSISH_TRACE_TRAPS) {
        return "traps";
    }
    return "misc";
}

static void trace_parse_env(void) {
    const char *env;
    char *copy;
    char *saveptr;
    char *token;

    if (g_trace_initialized) {
        return;
    }
    g_trace_initialized = true;
    g_trace_mask = 0;

    env = getenv("POSISH_TRACE");
    if (env == NULL || env[0] == '\0') {
        return;
    }

    copy = arena_alloc_in(NULL, strlen(env) + 1);
    strcpy(copy, env);

    saveptr = NULL;
    token = strtok_r(copy, ",:; \t", &saveptr);
    while (token != NULL) {
        if (token[0] == '\0') {
            token = strtok_r(NULL, ",:; \t", &saveptr);
            continue;
        }
        if (strcmp(token, "all") == 0) {
            g_trace_mask = trace_all_mask();
            break;
        } else if (strcmp(token, "signals") == 0) {
            g_trace_mask |= POSISH_TRACE_SIGNALS;
        } else if (strcmp(token, "jobs") == 0) {
            g_trace_mask |= POSISH_TRACE_JOBS;
        } else if (strcmp(token, "traps") == 0) {
            g_trace_mask |= POSISH_TRACE_TRAPS;
        }
        token = strtok_r(NULL, ",:; \t", &saveptr);
    }

    arena_maybe_free(copy);
}

bool trace_enabled(unsigned int category) {
    trace_parse_env();
    return (g_trace_mask & category) != 0;
}

void trace_log(unsigned int category, const char *fmt, ...) {
    va_list ap;

    if (!trace_enabled(category)) {
        return;
    }

    fprintf(stderr, "posish-trace[%ld][%s] ", (long)getpid(),
            trace_category_name(category));
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

#endif
