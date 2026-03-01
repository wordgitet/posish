#ifndef POSISH_TRACE_H
#define POSISH_TRACE_H

#include <stdbool.h>

#define POSISH_TRACE_SIGNALS (1u << 0)
#define POSISH_TRACE_JOBS (1u << 1)
#define POSISH_TRACE_TRAPS (1u << 2)

#if defined(POSISH_ENABLE_TRACE) && POSISH_ENABLE_TRACE
bool trace_enabled(unsigned int category);
void trace_log(unsigned int category, const char *fmt, ...);
#else
static inline bool trace_enabled(unsigned int category) {
    (void)category;
    return false;
}

static inline void trace_log(unsigned int category, const char *fmt, ...) {
    (void)category;
    (void)fmt;
}
#endif

#endif
