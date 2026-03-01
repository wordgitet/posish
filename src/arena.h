#ifndef POSISH_ARENA_H
#define POSISH_ARENA_H

#include <stddef.h>

void *arena_xmalloc(size_t size);
void *arena_xrealloc(void *ptr, size_t size);
char *arena_xstrdup(const char *s);

#endif
