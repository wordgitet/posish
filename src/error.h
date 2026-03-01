/* SPDX-License-Identifier: 0BSD */

/* posish - error reporting interface */

#ifndef POSISH_ERROR_H
#define POSISH_ERROR_H

#include <stddef.h>

void posish_errorf(const char *fmt, ...);
void posish_error_at(const char *source, size_t line, size_t col, const char *klass,
                     const char *fmt, ...);

#endif
