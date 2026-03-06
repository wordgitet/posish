/* SPDX-License-Identifier: 0BSD */

/* posish - error reporting interface */

#ifndef POSISH_ERROR_H
#define POSISH_ERROR_H

#include "error_catalog.h"

#include <stddef.h>

void posish_errorf(const char *fmt, ...);
void posish_error_at(const char *source, size_t line, size_t col, const char *klass,
                     const char *fmt, ...);
void posish_error_idf(enum posish_error_id id, ...);
void posish_error_at_idf(const char *source, size_t line, size_t col,
                         enum posish_error_id id, ...);

#endif
