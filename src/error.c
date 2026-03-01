#include "error.h"

#include <stdarg.h>
#include <stdio.h>

void posish_errorf(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    fputs("posish: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void posish_error_at(const char *source, size_t line, size_t col, const char *klass,
                     const char *fmt, ...) {
    va_list ap;

    if (source == NULL || source[0] == '\0') {
        source = "<input>";
    }
    if (klass == NULL || klass[0] == '\0') {
        klass = "error";
    }

    va_start(ap, fmt);
    fprintf(stderr, "posish: %s:%lu:%lu: %s: ", source, (unsigned long)line,
            (unsigned long)col, klass);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}
