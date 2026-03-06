/* SPDX-License-Identifier: 0BSD */

/* posish - error reporting */

#include "error.h"

#include <stdarg.h>
#include <stdio.h>

static void posish_verrorf(const char *fmt, va_list ap) {
  fputs("posish: ", stderr);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
}

static void posish_verror_at(const char *source, size_t line, size_t col,
                             const char *klass, const char *fmt, va_list ap) {
  if (source == NULL || source[0] == '\0') {
    source = "<input>";
  }
  if (klass == NULL || klass[0] == '\0') {
    klass = "error";
  }

  fprintf(stderr, "posish: %s:%lu:%lu: %s: ", source, (unsigned long)line,
          (unsigned long)col, klass);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
}

void posish_errorf(const char *fmt, ...) {
  va_list ap;

  va_start(ap, fmt);
  posish_verrorf(fmt, ap);
  va_end(ap);
}

void posish_error_at(const char *source, size_t line, size_t col,
                     const char *klass, const char *fmt, ...) {
  va_list ap;

  va_start(ap, fmt);
  posish_verror_at(source, line, col, klass, fmt, ap);
  va_end(ap);
}

void posish_error_idf(enum posish_error_id id, ...) {
  const struct posish_error_def *def;
  va_list ap;

  def = posish_error_lookup(id);
  if (def == NULL || def->fmt == NULL) {
    posish_errorf("unknown error id: %d", (int)id);
    return;
  }

  va_start(ap, id);
  posish_verrorf(def->fmt, ap);
  va_end(ap);
}

void posish_error_at_idf(const char *source, size_t line, size_t col,
                         enum posish_error_id id, ...) {
  const struct posish_error_def *def;
  va_list ap;

  def = posish_error_lookup(id);
  if (def == NULL || def->fmt == NULL) {
    posish_error_at(source, line, col, "error", "unknown error id: %d",
                    (int)id);
    return;
  }

  va_start(ap, id);
  posish_verror_at(source, line, col, posish_error_kind_name(def->kind),
                   def->fmt, ap);
  va_end(ap);
}
