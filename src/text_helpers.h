/* SPDX-License-Identifier: 0BSD */

/* posish - shared internal text helpers */

#ifndef POSISH_TEXT_HELPERS_H
#define POSISH_TEXT_HELPERS_H

#include <stdbool.h>
#include <stddef.h>

char *text_dup_trimmed_slice(const char *src, size_t start, size_t end);
char *text_dup_slice(const char *src, size_t start, size_t end);
bool text_is_name_start_char(char ch);
bool text_is_name_char(char ch);
bool text_keyword_boundary(char ch);
bool text_word_starts_command_position(const char *source, size_t pos);
size_t text_skip_continuations_forward(const char *source, size_t pos);

#endif
