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

static bool is_fully_quoted_pattern(const char *raw) {
  size_t len;
  size_t i;
  char q;

  len = strlen(raw);
  if (len < 2) {
    return false;
  }
  if ((raw[0] != '\'' && raw[0] != '"') || raw[len - 1] != raw[0]) {
    return false;
  }

  q = raw[0];
  i = 1;
  while (i + 1 < len) {
    if (q == '"' && raw[i] == '\\' && i + 2 < len) {
      i += 2;
      continue;
    }
    if (raw[i] == q) {
      return false;
    }
    i++;
  }
  return true;
}

static bool pattern_meta_char(char ch) {
  return ch == '*' || ch == '?' || ch == '[' || ch == ']' || ch == '\\';
}

static void push_char(char **buf, size_t *len, size_t *cap, char ch) {
  if (*len + 1 >= *cap) {
    if (*cap == 0) {
      *cap = 32;
    } else {
      *cap *= 2;
    }
    *buf = arena_xrealloc(*buf, *cap);
  }
  (*buf)[(*len)++] = ch;
}

static void push_literal_for_pattern(char **buf, size_t *len, size_t *cap,
                                     char ch) {
  if (pattern_meta_char(ch)) {
    push_char(buf, len, cap, '\\');
  }
  push_char(buf, len, cap, ch);
}

static char *raw_pattern_to_fnmatch(const char *raw) {
  char *out;
  size_t len;
  size_t cap;
  size_t i;
  char quote;

  out = NULL;
  len = 0;
  cap = 0;
  quote = '\0';

  for (i = 0; raw[i] != '\0'; i++) {
    char ch;

    ch = raw[i];
    if (quote == '\0') {
      if (ch == '\\') {
        if (raw[i + 1] != '\0') {
          i++;
          push_literal_for_pattern(&out, &len, &cap, raw[i]);
        } else {
          push_literal_for_pattern(&out, &len, &cap, '\\');
        }
        continue;
      }
      if (ch == '\'' || ch == '"') {
        quote = ch;
        continue;
      }
      push_char(&out, &len, &cap, ch);
      continue;
    }

    if ((quote == '\'' && ch == '\'') || (quote == '"' && ch == '"')) {
      quote = '\0';
      continue;
    }

    if (quote == '"' && ch == '\\' && raw[i + 1] != '\0') {
      i++;
      push_literal_for_pattern(&out, &len, &cap, raw[i]);
      continue;
    }

    push_literal_for_pattern(&out, &len, &cap, ch);
  }

  push_char(&out, &len, &cap, '\0');
  return out;
}

static bool raw_pattern_needs_word_expansion(const char *raw) {
  size_t i;

  for (i = 0; raw[i] != '\0'; i++) {
    if (raw[i] == '$' || raw[i] == '`' || raw[i] == '~') {
      return true;
    }
  }
  return false;
}

static char *escape_pattern_metacharacters(const char *pat) {
  size_t i;
  size_t len;
  size_t cap;
  char *out;

  len = strlen(pat);
  cap = len * 2 + 1;
  out = arena_xmalloc(cap);
  len = 0;

  for (i = 0; pat[i] != '\0'; i++) {
    if (pat[i] == '*' || pat[i] == '?' || pat[i] == '[' || pat[i] == ']' ||
        pat[i] == '\\') {
      out[len++] = '\\';
    }
    out[len++] = pat[i];
  }
  out[len] = '\0';
  return out;
}

static char *normalize_fnmatch_pattern(const char *pat) {
  size_t len;
  size_t trailing;
  char *out;

  len = strlen(pat);
  trailing = 0;
  while (trailing < len && pat[len - 1 - trailing] == '\\') {
    trailing++;
  }
  if ((trailing % 2) == 0) {
    return arena_xstrdup(pat);
  }

  out = arena_xmalloc(len + 2);
  memcpy(out, pat, len);
  out[len] = '\\';
  out[len + 1] = '\0';
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

static bool skip_backtick_subst(const char *src, size_t start, size_t *out_end) {
  size_t i;

  i = start + 1;
  while (src[i] != '\0') {
    if (src[i] == '\\' && src[i + 1] != '\0') {
      i += 2;
      continue;
    }
    if (src[i] == '`') {
      *out_end = i;
      return true;
    }
    i++;
  }
  return false;
}

static bool skip_braced_param(const char *src, size_t start, size_t *out_end) {
  size_t i;
  int depth;
  char quote;

  i = start + 2;
  depth = 1;
  quote = '\0';
  while (src[i] != '\0') {
    char ch;

    ch = src[i];
    if (quote == '\0') {
      if (ch == '\\' && src[i + 1] != '\0') {
        i += 2;
        continue;
      }
      if (ch == '\'' || ch == '"') {
        quote = ch;
        i++;
        continue;
      }
      if (ch == '{') {
        depth++;
      } else if (ch == '}') {
        depth--;
        if (depth == 0) {
          *out_end = i;
          return true;
        }
      }
    } else if ((quote == '\'' && ch == '\'') || (quote == '"' && ch == '"')) {
      quote = '\0';
    } else if (quote == '"' && ch == '\\' && src[i + 1] != '\0') {
      i++;
    }
    i++;
  }
  return false;
}

static bool skip_dollar_parens(const char *src, size_t start, size_t *out_end) {
  size_t i;
  int depth;
  char quote;

  i = start + 2;
  depth = 1;
  quote = '\0';
  while (src[i] != '\0') {
    char ch;

    ch = src[i];
    if (quote == '\0') {
      if (ch == '\\' && src[i + 1] != '\0') {
        i += 2;
        continue;
      }
      if (ch == '\'' || ch == '"') {
        quote = ch;
        i++;
        continue;
      }
      if (ch == '`') {
        size_t end;

        if (!skip_backtick_subst(src, i, &end)) {
          return false;
        }
        i = end + 1;
        continue;
      }
      if (ch == '$' && src[i + 1] == '{') {
        size_t end;

        if (!skip_braced_param(src, i, &end)) {
          return false;
        }
        i = end + 1;
        continue;
      }
      if (ch == '$' && src[i + 1] == '(') {
        size_t end;

        if (!skip_dollar_parens(src, i, &end)) {
          return false;
        }
        i = end + 1;
        continue;
      }
      if (ch == '(') {
        depth++;
      } else if (ch == ')') {
        depth--;
        if (depth == 0) {
          *out_end = i;
          return true;
        }
      }
    } else if ((quote == '\'' && ch == '\'') || (quote == '"' && ch == '"')) {
      quote = '\0';
    } else if (quote == '"' && ch == '\\' && src[i + 1] != '\0') {
      i++;
    }
    i++;
  }
  return false;
}

static bool find_pattern_clause_close(const char *source, size_t start,
                                      size_t *out_end) {
  size_t i;
  char quote;

  i = start;
  quote = '\0';
  while (source[i] != '\0') {
    char ch;

    ch = source[i];
    if (quote == '\0') {
      if (ch == '\\' && source[i + 1] != '\0') {
        i += 2;
        continue;
      }
      if (ch == '\'' || ch == '"') {
        quote = ch;
        i++;
        continue;
      }
      if (ch == '`') {
        size_t end;

        if (!skip_backtick_subst(source, i, &end)) {
          return false;
        }
        i = end + 1;
        continue;
      }
      if (ch == '$' && source[i + 1] == '{') {
        size_t end;

        if (!skip_braced_param(source, i, &end)) {
          return false;
        }
        i = end + 1;
        continue;
      }
      if (ch == '$' && source[i + 1] == '(') {
        size_t end;

        if (!skip_dollar_parens(source, i, &end)) {
          return false;
        }
        i = end + 1;
        continue;
      }
      if (ch == ')') {
        *out_end = i;
        return true;
      }
      i++;
      continue;
    }

    if ((quote == '\'' && ch == '\'') || (quote == '"' && ch == '"')) {
      quote = '\0';
    } else if (quote == '"' && ch == '\\' && source[i + 1] != '\0') {
      i++;
    }
    i++;
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
      char *match_pat;
      int rc;

      raw_pat = dup_trimmed_slice(pattern_list, start, i);
      if (!raw_pattern_needs_word_expansion(raw_pat)) {
        match_pat = raw_pattern_to_fnmatch(raw_pat);
      } else {
        if (expand_case_word(raw_pat, state, &pat) != 0) {
          free(raw_pat);
          return false;
        }
        if (is_fully_quoted_pattern(raw_pat)) {
          match_pat = escape_pattern_metacharacters(pat);
          free(pat);
        } else {
          match_pat = normalize_fnmatch_pattern(pat);
          free(pat);
        }
      }
      free(raw_pat);

      rc = fnmatch(match_pat, word, 0);
      free(match_pat);
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
  enum case_clause_term {
    CASE_TERM_END,
    CASE_TERM_DBL_SEMI,
    CASE_TERM_SEMI_AMP,
    CASE_TERM_DBL_SEMI_AMP
  };
  size_t i;
  size_t word_start;
  size_t word_end;
  bool found_in;
  char quote;
  char *word_expr;
  char *word;
  int status;
  bool matched;
  bool force_execute;
  bool test_after_match;

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
  force_execute = false;
  test_after_match = false;
  status = 0;
  while (1) {
    size_t pat_start;
    size_t pat_end;
    size_t cmd_start;
    size_t cmd_end;
    char *patterns;
    bool clause_ended_with_esac;
    enum case_clause_term terminator;

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
    if (!find_pattern_clause_close(source, i, &pat_end)) {
      free(word);
      *status_out = 2;
      return true;
    }
    i = pat_end + 1;

    while (isspace((unsigned char)source[i])) {
      i++;
    }

    cmd_start = i;
    quote = '\0';
    clause_ended_with_esac = false;
    terminator = CASE_TERM_END;
    {
      int nested_case_depth;

      nested_case_depth = 0;
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
          if ((isalpha((unsigned char)ch) || ch == '_') &&
              word_starts_command_position(source, i)) {
            if (strncmp(source + i, "case", 4) == 0 &&
                keyword_boundary(source[i + 4])) {
              nested_case_depth++;
              i += 3;
              continue;
            }
            if (strncmp(source + i, "esac", 4) == 0 &&
                keyword_boundary(source[i + 4])) {
              if (nested_case_depth == 0) {
                clause_ended_with_esac = true;
                break;
              }
              nested_case_depth--;
              i += 3;
              continue;
            }
          }
          if (nested_case_depth == 0 && ch == ';' && source[i + 1] == ';' &&
              source[i + 2] == '&') {
            terminator = CASE_TERM_DBL_SEMI_AMP;
            break;
          }
          if (nested_case_depth == 0 && ch == ';' && source[i + 1] == ';') {
            terminator = CASE_TERM_DBL_SEMI;
            break;
          }
          if (nested_case_depth == 0 && ch == ';' && source[i + 1] == '&') {
            terminator = CASE_TERM_SEMI_AMP;
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
    }
    if (source[i] == '\0' && !clause_ended_with_esac) {
      free(word);
      *status_out = 2;
      return true;
    }

    cmd_end = i;
    patterns = dup_trimmed_slice(source, pat_start, pat_end);
    {
      bool clause_selected;

      if (force_execute) {
        clause_selected = true;
      } else if (!matched || test_after_match) {
        clause_selected = case_pattern_list_matches(state, patterns, word);
      } else {
        clause_selected = false;
      }

      if (clause_selected) {
      char *body;

      body = dup_trimmed_slice(source, cmd_start, cmd_end);
      if (body[0] != '\0') {
        status = runner(state, body);
      }
      free(body);
      matched = true;
      if (clause_ended_with_esac || terminator == CASE_TERM_END) {
        free(patterns);
        break;
      }
      if (terminator == CASE_TERM_DBL_SEMI) {
        free(patterns);
        break;
      }
      if (terminator == CASE_TERM_SEMI_AMP) {
        force_execute = true;
        test_after_match = false;
      } else if (terminator == CASE_TERM_DBL_SEMI_AMP) {
        force_execute = false;
        test_after_match = true;
      }
      } else {
        force_execute = false;
        test_after_match = false;
      }
    }
    free(patterns);

    if (terminator == CASE_TERM_DBL_SEMI_AMP) {
      i += 3;
    } else if (terminator == CASE_TERM_DBL_SEMI ||
               terminator == CASE_TERM_SEMI_AMP) {
      i += 2;
    }
  }

  free(word);
  *status_out = status;
  return true;
}
