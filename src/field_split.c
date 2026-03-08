/* SPDX-License-Identifier: 0BSD */

/* posish - field splitting helpers */

#include "field_split.h"

#include "arena.h"
#include "expand_markers.h"
#include "vars.h"

#include <stdlib.h>
#include <string.h>

static void scratch_dispose(void *ptr) {
  if (ptr == NULL) {
    return;
  }
  if (arena_get_current() == NULL) {
    heap_free(ptr);
  }
}

static bool is_ifs_char(const char *ifs, char ch) {
  size_t i;

  for (i = 0; ifs[i] != '\0'; i++) {
    if (ifs[i] == ch) {
      return true;
    }
  }
  return false;
}

static bool is_ifs_whitespace_char(const char *ifs, char ch) {
  if (ch != ' ' && ch != '\t' && ch != '\n') {
    return false;
  }
  return is_ifs_char(ifs, ch);
}

static bool is_split_delimiter(const char *ifs, char ch) {
  if (ch == QUOTED_IFS_SPACE || ch == QUOTED_IFS_TAB ||
      ch == QUOTED_IFS_NEWLINE) {
    return false;
  }
  return is_ifs_char(ifs, ch);
}

void field_split_restore_quoted_markers(char *s) {
  size_t i;
  size_t j;

  j = 0;
  for (i = 0; s[i] != '\0'; i++) {
    char ch;

    ch = s[i];
    if (ch == QUOTED_LITERAL_PREFIX && s[i + 1] != '\0') {
      s[j++] = s[i + 1];
      i++;
      continue;
    }
    if (ch == QUOTED_EMPTY_MARK) {
      continue;
    }
    if (ch == QUOTED_IFS_SPACE) {
      ch = ' ';
    } else if (ch == QUOTED_IFS_TAB) {
      ch = '\t';
    } else if (ch == QUOTED_IFS_NEWLINE) {
      ch = '\n';
    } else if (ch == QUOTED_GLOB_STAR) {
      ch = '*';
    } else if (ch == QUOTED_GLOB_QMARK) {
      ch = '?';
    } else if (ch == QUOTED_GLOB_LBRACK) {
      ch = '[';
    }
    s[j++] = ch;
  }
  s[j] = '\0';
}

int field_split_split(const char *expanded, struct token_vec *out,
                      const struct shell_state *state) {
  const char *ifs_env;
  const char *ifs;
  size_t pos;
  int appended;
  bool has_delimiter;

  ifs_env = vars_get(state, "IFS");
  if (ifs_env == NULL) {
    ifs = " \t\n";
  } else {
    ifs = ifs_env;
  }

  if (ifs[0] == '\0') {
    return 0;
  }

  has_delimiter = false;
  for (pos = 0; expanded[pos] != '\0'; pos++) {
    if (expanded[pos] == QUOTED_LITERAL_PREFIX && expanded[pos + 1] != '\0') {
      pos++;
      continue;
    }
    if (is_split_delimiter(ifs, expanded[pos])) {
      has_delimiter = true;
      break;
    }
  }
  if (!has_delimiter) {
    return 0;
  }

  pos = 0;
  appended = 0;
  while (expanded[pos] != '\0') {
    size_t start;
    size_t end;
    char *field;

    while (expanded[pos] != '\0' &&
           is_ifs_whitespace_char(ifs, expanded[pos])) {
      pos++;
    }
    if (expanded[pos] == '\0') {
      break;
    }

    start = pos;
    while (expanded[pos] != '\0') {
      if (expanded[pos] == QUOTED_LITERAL_PREFIX &&
          expanded[pos + 1] != '\0') {
        pos += 2;
        continue;
      }
      if (is_split_delimiter(ifs, expanded[pos])) {
        break;
      }
      pos++;
    }
    end = pos;

    field = arena_xmalloc((end - start) + 1);
    memcpy(field, expanded + start, end - start);
    field[end - start] = '\0';
    field_split_restore_quoted_markers(field);
    out->items = arena_xrealloc(out->items, sizeof(*out->items) * (out->len + 1));
    out->items[out->len++] = field;
    appended++;

    if (expanded[pos] == '\0') {
      break;
    }

    while (expanded[pos] != '\0') {
      if (expanded[pos] == QUOTED_LITERAL_PREFIX && expanded[pos + 1] != '\0') {
        break;
      }
      if (!is_split_delimiter(ifs, expanded[pos])) {
        break;
      }
      if (!is_ifs_whitespace_char(ifs, expanded[pos])) {
        pos++;
        while (expanded[pos] != '\0' &&
               is_ifs_whitespace_char(ifs, expanded[pos])) {
          pos++;
        }
        break;
      }
      pos++;
    }
  }

  return appended;
}

bool field_split_has_delimiter(const char *expanded,
                               const struct shell_state *state) {
  const char *ifs_env;
  const char *ifs;
  size_t pos;

  ifs_env = vars_get(state, "IFS");
  if (ifs_env == NULL) {
    ifs = " \t\n";
  } else {
    ifs = ifs_env;
  }
  if (ifs[0] == '\0') {
    return false;
  }

  for (pos = 0; expanded[pos] != '\0'; pos++) {
    if (expanded[pos] == QUOTED_LITERAL_PREFIX && expanded[pos + 1] != '\0') {
      pos++;
      continue;
    }
    if (is_split_delimiter(ifs, expanded[pos])) {
      return true;
    }
  }
  return false;
}

bool field_split_has_at_marker(const char *expanded) {
  size_t i;

  for (i = 0; expanded[i] != '\0'; i++) {
    if (expanded[i] == PARAM_AT_SPLIT) {
      return true;
    }
  }
  return false;
}

int field_split_append_piece(char *piece, struct token_vec *out,
                             bool split_fields,
                             const struct shell_state *state) {
  if (split_fields) {
    int count;
    bool had_delim;

    had_delim = field_split_has_delimiter(piece, state);
    count = field_split_split(piece, out, state);
    if (count > 0) {
      scratch_dispose(piece);
      return count;
    }
    if (had_delim) {
      scratch_dispose(piece);
      return 0;
    }
  }

  field_split_restore_quoted_markers(piece);
  out->items = arena_xrealloc(out->items, sizeof(*out->items) * (out->len + 1));
  out->items[out->len++] = piece;
  return 1;
}

int field_split_append_at_expansion(const char *expanded, struct token_vec *out,
                                    bool split_fields,
                                    const struct shell_state *state) {
  size_t start;
  size_t i;
  int added_total;

  start = 0;
  added_total = 0;
  for (i = 0;; i++) {
    if (expanded[i] != PARAM_AT_SPLIT && expanded[i] != '\0') {
      continue;
    }

    {
      char *piece;
      size_t plen;

      plen = i - start;
      piece = arena_xmalloc(plen + 1);
      memcpy(piece, expanded + start, plen);
      piece[plen] = '\0';
      added_total += field_split_append_piece(piece, out, split_fields, state);
    }

    if (expanded[i] == '\0') {
      break;
    }
    start = i + 1;
  }

  return added_total;
}
