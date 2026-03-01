/* SPDX-License-Identifier: 0BSD */

/* posish - case command handling */

#include "case_command.h"

#include "arena.h"
#include "expand.h"
#include "lexer.h"

#include <ctype.h>
#include <fnmatch.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static char *dup_trimmed_slice(const char *src, size_t start, size_t end) {
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

static bool keyword_boundary(char ch) {
  return ch == '\0' || isspace((unsigned char)ch) || ch == ';' || ch == '&' ||
         ch == '|' || ch == '(' || ch == ')' || ch == '{' || ch == '}';
}

static bool word_starts_command_position(const char *source, size_t pos) {
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
        ch == '{') {
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
        (len == 4 && strncmp(source + start, "elif", 4) == 0)) {
      return true;
    }
  }

  return false;
}

static int expand_case_word(const char *expr, struct shell_state *state,
                            char **out_word) {
  struct token_vec lexed;
  struct token_vec expanded;
  size_t i;
  size_t total_len;
  char *joined;

  lexed.items = NULL;
  lexed.len = 0;
  expanded.items = NULL;
  expanded.len = 0;

  if (lexer_split_words(expr, &lexed) != 0) {
    return 2;
  }
  if (expand_words(&lexed, &expanded, state, false) != 0) {
    lexer_free_tokens(&lexed);
    return 2;
  }
  lexer_free_tokens(&lexed);

  if (expanded.len == 0) {
    *out_word = arena_xstrdup("");
    lexer_free_tokens(&expanded);
    return 0;
  }
  if (expanded.len == 1) {
    *out_word = arena_xstrdup(expanded.items[0]);
    lexer_free_tokens(&expanded);
    return 0;
  }

  total_len = 0;
  for (i = 0; i < expanded.len; i++) {
    total_len += strlen(expanded.items[i]);
    if (i + 1 < expanded.len) {
      total_len++;
    }
  }

  joined = arena_xmalloc(total_len + 1);
  joined[0] = '\0';
  for (i = 0; i < expanded.len; i++) {
    if (i > 0) {
      strcat(joined, " ");
    }
    strcat(joined, expanded.items[i]);
  }

  *out_word = joined;
  lexer_free_tokens(&expanded);
  return 0;
}

static bool case_pattern_list_matches(struct shell_state *state,
                                      const char *pattern_list,
                                      const char *word) {
  size_t i;
  size_t start;
  char quote;

  i = 0;
  start = 0;
  quote = '\0';
  for (;; i++) {
    char ch;
    bool delim;

    ch = pattern_list[i];
    delim = false;

    if (ch == '\0') {
      delim = true;
    } else if (quote == '\0') {
      if (ch == '\\' && pattern_list[i + 1] != '\0') {
        i++;
        continue;
      }
      if (ch == '\'' || ch == '"') {
        quote = ch;
        continue;
      }
      if (ch == '|') {
        delim = true;
      }
    } else if (quote == '\'' && ch == '\'') {
      quote = '\0';
    } else if (quote == '"') {
      if (ch == '\\' && pattern_list[i + 1] != '\0') {
        i++;
        continue;
      }
      if (ch == '"') {
        quote = '\0';
      }
    }

    if (delim) {
      char *raw_pat;
      char *pat;
      int rc;

      raw_pat = dup_trimmed_slice(pattern_list, start, i);
      if (expand_case_word(raw_pat, state, &pat) != 0) {
        free(raw_pat);
        return false;
      }
      free(raw_pat);

      rc = fnmatch(pat, word, 0);
      free(pat);
      if (rc == 0) {
        return true;
      }

      if (ch == '\0') {
        break;
      }
      start = i + 1;
    }
  }
  return false;
}

bool try_execute_case_command(struct shell_state *state, const char *source,
                              int *status_out, case_command_runner_fn runner) {
  size_t i;
  size_t word_start;
  size_t word_end;
  bool found_in;
  char quote;
  char *word_expr;
  char *word;
  int status;
  bool matched;

  i = 0;
  while (isspace((unsigned char)source[i])) {
    i++;
  }
  if (strncmp(source + i, "case", 4) != 0 || !keyword_boundary(source[i + 4])) {
    return false;
  }
  i += 4;

  while (isspace((unsigned char)source[i])) {
    i++;
  }
  word_start = i;
  word_end = i;
  found_in = false;
  quote = '\0';

  for (; source[i] != '\0'; i++) {
    char ch;

    ch = source[i];
    if (quote == '\0') {
      if (ch == '\\' && source[i + 1] != '\0') {
        i++;
        continue;
      }
      if (ch == '\'' || ch == '"') {
        quote = ch;
        continue;
      }
      if (isspace((unsigned char)ch)) {
        size_t j;

        j = i;
        while (isspace((unsigned char)source[j])) {
          j++;
        }
        if (strncmp(source + j, "in", 2) == 0 &&
            keyword_boundary(source[j + 2])) {
          word_end = i;
          i = j + 2;
          found_in = true;
          break;
        }
      }
    } else if (quote == '\'' && ch == '\'') {
      quote = '\0';
    } else if (quote == '"') {
      if (ch == '\\' && source[i + 1] != '\0') {
        i++;
        continue;
      }
      if (ch == '"') {
        quote = '\0';
      }
    }
  }

  if (!found_in) {
    return false;
  }

  word_expr = dup_trimmed_slice(source, word_start, word_end);
  if (expand_case_word(word_expr, state, &word) != 0) {
    free(word_expr);
    *status_out = 2;
    return true;
  }
  free(word_expr);

  matched = false;
  status = 0;
  while (1) {
    size_t pat_start;
    size_t pat_end;
    size_t cmd_start;
    size_t cmd_end;
    char *patterns;
    bool clause_ended_with_esac;
    bool clause_ended_with_terminator;

    while (isspace((unsigned char)source[i])) {
      i++;
    }

    if (strncmp(source + i, "esac", 4) == 0 &&
        keyword_boundary(source[i + 4])) {
      size_t j;
      /* Ignore any trailing whitespace after the esac */
      j = i + 4;
      while (isspace((unsigned char)source[j])) {
        j++;
      }
      if (source[j] != '\0') {
        free(word);
        *status_out = 2;
        return true;
      }
      break;
    }

    if (source[i] == '(') {
      i++;
    }
    pat_start = i;
    quote = '\0';
    for (; source[i] != '\0'; i++) {
      char ch;

      ch = source[i];
      if (quote == '\0') {
        if (ch == '\\' && source[i + 1] != '\0') {
          i++;
          continue;
        }
        if (ch == '\'' || ch == '"') {
          quote = ch;
          continue;
        }
        if (ch == ')') {
          break;
        }
      } else if (quote == '\'' && ch == '\'') {
        quote = '\0';
      } else if (quote == '"') {
        if (ch == '\\' && source[i + 1] != '\0') {
          i++;
          continue;
        }
        if (ch == '"') {
          quote = '\0';
        }
      }
    }
    if (source[i] != ')') {
      free(word);
      *status_out = 2;
      return true;
    }
    pat_end = i;
    i++;

    while (isspace((unsigned char)source[i])) {
      i++;
    }

    cmd_start = i;
    quote = '\0';
    clause_ended_with_esac = false;
    clause_ended_with_terminator = false;
    for (; source[i] != '\0'; i++) {
      char ch;

      ch = source[i];
      if (quote == '\0') {
        if (ch == '\\' && source[i + 1] != '\0') {
          i++;
          continue;
        }
        if (ch == '\'' || ch == '"') {
          quote = ch;
          continue;
        }
        if (ch == ';' && source[i + 1] == ';') {
          clause_ended_with_terminator = true;
          break;
        }
        if ((isalpha((unsigned char)ch) || ch == '_') &&
            word_starts_command_position(source, i) &&
            strncmp(source + i, "esac", 4) == 0 &&
            keyword_boundary(source[i + 4])) {
          clause_ended_with_esac = true;
          break;
        }
      } else if (quote == '\'' && ch == '\'') {
        quote = '\0';
      } else if (quote == '"') {
        if (ch == '\\' && source[i + 1] != '\0') {
          i++;
          continue;
        }
        if (ch == '"') {
          quote = '\0';
        }
      }
    }
    if (source[i] == '\0' && !clause_ended_with_esac) {
      free(word);
      *status_out = 2;
      return true;
    }

    cmd_end = i;
    patterns = dup_trimmed_slice(source, pat_start, pat_end);
    if (!matched && case_pattern_list_matches(state, patterns, word)) {
      char *body;

      body = dup_trimmed_slice(source, cmd_start, cmd_end);
      if (body[0] == '\0') {
        status = 0;
      } else {
        status = runner(state, body);
      }
      free(body);
      matched = true;
    }
    free(patterns);

    if (clause_ended_with_terminator) {
      i += 2;
    }
  }

  free(word);
  *status_out = status;
  return true;
}
