/* SPDX-License-Identifier: 0BSD */

/* posish - shared internal text helpers */

#include "text_helpers.h"

#include "arena.h"

#include <ctype.h>
#include <string.h>

char *text_dup_trimmed_slice(const char *src, size_t start, size_t end) {
  char *out;
  size_t len;

  while (start < end && isspace((unsigned char)src[start])) {
    start++;
  }
  while (end > start && isspace((unsigned char)src[end - 1])) {
    end--;
  }

  len = end - start;
  out = arena_xmalloc(len + 1);
  if (len > 0) {
    memcpy(out, src + start, len);
  }
  out[len] = '\0';
  return out;
}

char *text_dup_slice(const char *src, size_t start, size_t end) {
  char *out;
  size_t len;

  len = end - start;
  out = arena_xmalloc(len + 1);
  if (len > 0) {
    memcpy(out, src + start, len);
  }
  out[len] = '\0';
  return out;
}

bool text_is_name_start_char(char ch) {
  return isalpha((unsigned char)ch) || ch == '_';
}

bool text_is_name_char(char ch) {
  return isalnum((unsigned char)ch) || ch == '_';
}

bool text_keyword_boundary(char ch) {
  return ch == '\0' || isspace((unsigned char)ch) || ch == ';' || ch == '&' ||
         ch == '|' || ch == '(' || ch == ')' || ch == '{' || ch == '}';
}

bool text_word_starts_command_position(const char *source, size_t pos) {
  size_t i;

  if (pos == 0) {
    return true;
  }

  i = pos;
  while (i > 0) {
    char ch;

    ch = source[i - 1];
    if (ch == ' ' || ch == '\t') {
      i--;
      continue;
    }
    if (ch == '\n' || ch == ';' || ch == '&' || ch == '|' || ch == '(' ||
        ch == ')' || ch == '{' || ch == '}') {
      return true;
    }
    break;
  }

  if (i == 0) {
    return true;
  }

  if (isalnum((unsigned char)source[i - 1]) || source[i - 1] == '_') {
    size_t start;
    size_t len;

    start = i - 1;
    while (start > 0 && (isalnum((unsigned char)source[start - 1]) ||
                         source[start - 1] == '_')) {
      start--;
    }
    len = i - start;
    if ((len == 4 && strncmp(source + start, "then", 4) == 0) ||
        (len == 2 && strncmp(source + start, "do", 2) == 0) ||
        (len == 4 && strncmp(source + start, "else", 4) == 0) ||
        (len == 4 && strncmp(source + start, "elif", 4) == 0) ||
        (len == 2 && strncmp(source + start, "if", 2) == 0) ||
        (len == 2 && strncmp(source + start, "fi", 2) == 0)) {
      return true;
    }
  }

  return false;
}

size_t text_skip_continuations_forward(const char *source, size_t pos) {
  while (source[pos] == '\\' && source[pos + 1] == '\n') {
    pos += 2;
  }
  return pos;
}
