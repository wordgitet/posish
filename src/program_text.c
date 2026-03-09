/* SPDX-License-Identifier: 0BSD */

/* posish - text-mode program execution helpers */

#include "program_text.h"

#include "alias.h"
#include "arena.h"
#include "compound_parse.h"
#include "exec.h"
#include "jobs.h"
#include "text_helpers.h"
#include "vars.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

enum andor_op { ANDOR_AND, ANDOR_OR };

struct strip_heredoc_marker {
  char *delimiter;
  bool strip_tabs;
};

static void *xrealloc(void *ptr, size_t size) {
  return arena_xrealloc(ptr, size);
}

char *program_text_collapse_line_continuations(const char *source) {
  size_t i;
  size_t j;
  size_t slen;
  char quote;
  char *out;

  slen = strlen(source);
  out = arena_xmalloc(slen + 1);
  quote = '\0';
  j = 0;

  for (i = 0; source[i] != '\0'; i++) {
    char ch;

    ch = source[i];
    if (quote == '\'') {
      out[j++] = ch;
      if (ch == '\'') {
        quote = '\0';
      }
      continue;
    }

    if (ch == '\\' && source[i + 1] == '\n') {
      i++;
      continue;
    }

    if (quote == '"') {
      out[j++] = ch;
      if (ch == '\\' && source[i + 1] != '\0' && source[i + 1] != '\n') {
        out[j++] = source[++i];
        continue;
      }
      if (ch == '"') {
        quote = '\0';
      }
      continue;
    }

    out[j++] = ch;
    if (ch == '\\' && source[i + 1] != '\0') {
      out[j++] = source[++i];
      continue;
    }
    if (ch == '\'' || ch == '"') {
      quote = ch;
    }
  }

  out[j] = '\0';
  return out;
}

static size_t source_line_at_offset(const char *source, size_t offset) {
  size_t i;
  size_t line;

  line = 1;
  for (i = 0; source[i] != '\0' && i < offset; i++) {
    if (source[i] == '\n') {
      line++;
    }
  }
  return line;
}

static void set_lineno_for_command(struct shell_state *state,
                                   const char *source, size_t start) {
  const char *base_text;
  char *end;
  unsigned long base;
  char line_buf[32];
  size_t line;

  base = 0;
  base_text = getenv("POSISH_LINENO_BASE");
  if (base_text != NULL && base_text[0] != '\0') {
    errno = 0;
    base = strtoul(base_text, &end, 10);
    if (errno != 0 || end == base_text || *end != '\0') {
      base = 0;
    }
  }

  line = source_line_at_offset(source, start) + (size_t)base;
  snprintf(line_buf, sizeof(line_buf), "%zu", line);
  (void)vars_set_with_mode(state, "LINENO", line_buf, false, false);
}

static void free_string_vec(char **vec, size_t len) {
  size_t i;

  if (vec == NULL) {
    return;
  }
  for (i = 0; i < len; i++) {
    arena_maybe_free(vec[i]);
  }
  arena_maybe_free(vec);
}

static void free_strip_heredoc_markers(struct strip_heredoc_marker *markers,
                                       size_t count) {
  size_t i;

  if (markers == NULL) {
    return;
  }
  for (i = 0; i < count; i++) {
    arena_maybe_free(markers[i].delimiter);
  }
  arena_maybe_free(markers);
}

static char *unquote_strip_heredoc_delimiter(const char *raw, size_t len) {
  size_t i;
  size_t out_len;
  char *out;

  out = arena_xmalloc(len + 1);
  out_len = 0;
  i = 0;
  while (i < len) {
    char ch;

    ch = raw[i];
    if (ch == '\\' && i + 1 < len) {
      out[out_len++] = raw[i + 1];
      i += 2;
      continue;
    }
    if (ch == '\'' || ch == '"') {
      char q;

      q = ch;
      i++;
      while (i < len && raw[i] != q) {
        if (q == '"' && raw[i] == '\\' && i + 1 < len) {
          out[out_len++] = raw[i + 1];
          i += 2;
          continue;
        }
        out[out_len++] = raw[i];
        i++;
      }
      if (i < len && raw[i] == q) {
        i++;
      }
      continue;
    }
    out[out_len++] = ch;
    i++;
  }

  out[out_len] = '\0';
  return out;
}

static int push_strip_heredoc_marker(struct strip_heredoc_marker **markers,
                                     size_t *count, size_t *cap,
                                     const char *delimiter_raw,
                                     size_t delimiter_raw_len,
                                     bool strip_tabs) {
  struct strip_heredoc_marker *grown;
  char *unquoted;

  if (*count == *cap) {
    size_t new_cap;

    new_cap = *cap == 0 ? 4 : *cap * 2;
    grown = xrealloc(*markers, sizeof(**markers) * new_cap);
    *markers = grown;
    *cap = new_cap;
  }

  unquoted = unquote_strip_heredoc_delimiter(delimiter_raw, delimiter_raw_len);
  if (unquoted[0] == '\0') {
    arena_maybe_free(unquoted);
    return -1;
  }

  (*markers)[*count].delimiter = unquoted;
  (*markers)[*count].strip_tabs = strip_tabs;
  (*count)++;
  return 0;
}

static char *strip_comments(const char *src) {
  size_t i;
  size_t j;
  char quote;
  char prev;
  int param_depth;
  struct strip_heredoc_marker *markers;
  size_t marker_count;
  size_t marker_cap;
  size_t marker_idx;
  bool in_heredoc_body;
  char *out;

  out = arena_xmalloc(strlen(src) + 1);
  quote = '\0';
  prev = '\0';
  param_depth = 0;
  markers = NULL;
  marker_count = 0;
  marker_cap = 0;
  marker_idx = 0;
  in_heredoc_body = false;
  i = 0;
  j = 0;

  while (src[i] != '\0') {
    char ch;

    ch = src[i];

    if (in_heredoc_body) {
      size_t line_start;
      size_t line_end;
      size_t cmp_start;
      size_t delim_len;
      bool delimiter_match;

      line_start = i;
      while (src[i] != '\0' && src[i] != '\n') {
        i++;
      }
      line_end = i;

      cmp_start = line_start;
      if (markers[marker_idx].strip_tabs) {
        while (cmp_start < line_end && src[cmp_start] == '\t') {
          cmp_start++;
        }
      }

      delim_len = strlen(markers[marker_idx].delimiter);
      delimiter_match = line_end - cmp_start == delim_len &&
                        memcmp(src + cmp_start, markers[marker_idx].delimiter,
                               delim_len) == 0;

      memcpy(out + j, src + line_start, line_end - line_start);
      j += line_end - line_start;

      if (src[i] == '\n') {
        out[j++] = src[i++];
        prev = '\n';
      } else {
        prev = line_end > line_start ? src[line_end - 1] : prev;
      }

      if (delimiter_match) {
        marker_idx++;
        if (marker_idx >= marker_count) {
          free_strip_heredoc_markers(markers, marker_count);
          markers = NULL;
          marker_count = 0;
          marker_cap = 0;
          marker_idx = 0;
          in_heredoc_body = false;
        }
      }
      continue;
    }

    if (quote == '\0') {
      if (ch == '$' && src[i + 1] == '\'') {
        out[j++] = src[i++];
        out[j++] = src[i++];
        while (src[i] != '\0') {
          out[j++] = src[i];
          if (src[i] == '\\' && src[i + 1] != '\0') {
            i++;
            out[j++] = src[i];
            i++;
            continue;
          }
          if (src[i] == '\'') {
            i++;
            break;
          }
          i++;
        }
        prev = out[j - 1];
        continue;
      }
      if (ch == '$' && src[i + 1] == '{') {
        out[j++] = src[i++];
        out[j++] = src[i++];
        param_depth++;
        prev = out[j - 1];
        continue;
      }
      if (param_depth > 0) {
        if (ch == '{') {
          param_depth++;
        } else if (ch == '}') {
          param_depth--;
        }
      }
      if (ch == '\\' && src[i + 1] != '\0') {
        out[j++] = src[i++];
        out[j++] = src[i++];
        prev = out[j - 1];
        continue;
      }
      if (ch == '<' && src[i + 1] == '<') {
        size_t op_start;
        bool strip_tabs;
        size_t delim_start;
        size_t delim_end;
        size_t k;

        op_start = i;
        i += 2;
        strip_tabs = false;
        if (src[i] == '-') {
          strip_tabs = true;
          i++;
        }
        while (src[i] == ' ' || src[i] == '\t') {
          i++;
        }
        delim_start = i;
        while (src[i] != '\0') {
          if (isspace((unsigned char)src[i]) || src[i] == ';' ||
              src[i] == '&' || src[i] == '|' || src[i] == '<' ||
              src[i] == '>') {
            break;
          }
          if (src[i] == '\\' && src[i + 1] != '\0') {
            i += 2;
            continue;
          }
          if (src[i] == '\'' || src[i] == '"') {
            char q;

            q = src[i++];
            while (src[i] != '\0' && src[i] != q) {
              if (q == '"' && src[i] == '\\' && src[i + 1] != '\0') {
                i += 2;
                continue;
              }
              i++;
            }
            if (src[i] == q) {
              i++;
            }
            continue;
          }
          i++;
        }
        delim_end = i;

        if (delim_end > delim_start) {
          push_strip_heredoc_marker(&markers, &marker_count, &marker_cap,
                                    src + delim_start, delim_end - delim_start,
                                    strip_tabs);
        }

        for (k = op_start; k < i; k++) {
          out[j++] = src[k];
        }
        prev = out[j - 1];
        continue;
      }
      if (ch == '\'' || ch == '"') {
        quote = ch;
        out[j++] = src[i++];
        prev = ch;
        continue;
      }
      if (ch == '#' && param_depth == 0) {
        bool comment_start;

        comment_start = prev == '\0' || isspace((unsigned char)prev) ||
                        strchr("|&;()<>", prev) != NULL;
        if (comment_start) {
          while (src[i] != '\0' && src[i] != '\n') {
            i++;
          }
          continue;
        }
      }
    } else if (quote == '\'' && ch == '\'') {
      quote = '\0';
    } else if (quote == '"') {
      if (ch == '\\' && src[i + 1] != '\0') {
        out[j++] = src[i++];
        out[j++] = src[i++];
        prev = out[j - 1];
        continue;
      }
      if (ch == '"') {
        quote = '\0';
      }
    }

    out[j++] = src[i++];
    prev = out[j - 1];

    if (ch == '\n' && marker_count > 0 && marker_idx == 0) {
      in_heredoc_body = true;
    }
  }

  free_strip_heredoc_markers(markers, marker_count);
  out[j] = '\0';
  return out;
}

static bool keyword_preceded_by_list_separator(const char *source, size_t pos) {
  size_t i;
  char ch;

  i = pos;
  while (i > 0 && (source[i - 1] == ' ' || source[i - 1] == '\t')) {
    i--;
  }
  if (i == 0) {
    return true;
  }

  ch = source[i - 1];
  return ch == '\n' || ch == ';' || ch == '&' || ch == '|' || ch == '(' ||
         ch == ')' || ch == '{' || ch == '}';
}

static bool newline_continues_command(const char *source, size_t len,
                                      size_t pos) {
  size_t i;

  if (source[pos] != '\n') {
    return false;
  }

  i = pos;
  while (i > 0) {
    char ch;

    if (shell_position_in_comment(source, len, i - 1)) {
      i--;
      continue;
    }
    ch = source[i - 1];
    if (ch == ' ' || ch == '\t' || ch == '\n') {
      i--;
      continue;
    }
    break;
  }
  if (i == 0) {
    return false;
  }

  if (source[i - 1] == '|') {
    return true;
  }
  if (source[i - 1] == '&' && i >= 2 && source[i - 2] == '&') {
    return true;
  }
  return false;
}

static bool command_requires_program_runner(const char *source) {
  size_t i;
  char quote;

  quote = '\0';
  for (i = 0; source[i] != '\0'; i++) {
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
      if (ch == ';' || ch == '\n') {
        return true;
      }
      continue;
    }

    if (quote == '\'' && ch == '\'') {
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

  return false;
}

static long previous_logical_index(const char *source, size_t pos) {
  long i;

  i = (long)pos - 1;
  while (i >= 1 && source[i - 1] == '\\' && source[i] == '\n') {
    i -= 2;
  }
  return i;
}

static bool is_async_separator_amp(const char *source, size_t pos) {
  size_t next;
  long prev;

  if (source[pos] != '&') {
    return false;
  }

  next = text_skip_continuations_forward(source, pos + 1);
  if (source[next] == '&') {
    return false;
  }

  prev = previous_logical_index(source, pos);
  if (prev >= 0 &&
      (source[prev] == '&' || source[prev] == '<' || source[prev] == '>')) {
    return false;
  }
  return true;
}

static bool find_command_subst_end(const char *source, size_t start,
                                   size_t *end_out) {
  size_t i;
  int depth;
  char quote;

  if (source[start] != '$' || source[start + 1] != '(') {
    return false;
  }

  depth = 1;
  quote = '\0';
  for (i = start + 2; source[i] != '\0'; i++) {
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
      if (ch == '$' && source[i + 1] == '(') {
        depth++;
        i++;
        continue;
      }
      if (ch == '(') {
        depth++;
        continue;
      }
      if (ch == ')') {
        depth--;
        if (depth == 0) {
          *end_out = i;
          return true;
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

  return false;
}

static bool find_dollar_single_quote_end(const char *source, size_t start,
                                         size_t *end_out) {
  size_t i;

  if (source[start] != '$' || source[start + 1] != '\'') {
    return false;
  }

  for (i = start + 2; source[i] != '\0'; i++) {
    if (source[i] == '\\' && source[i + 1] != '\0') {
      i++;
      continue;
    }
    if (source[i] == '\'') {
      *end_out = i;
      return true;
    }
  }

  return false;
}

static bool looks_like_function_header_only(const char *source) {
  size_t i;
  char *cleaned;
  bool result;

  cleaned = strip_comments(source);
  i = 0;
  while (isspace((unsigned char)cleaned[i])) {
    i++;
  }
  if (!text_is_name_start_char(cleaned[i])) {
    arena_maybe_free(cleaned);
    return false;
  }
  i++;
  while (text_is_name_char(cleaned[i])) {
    i++;
  }
  while (isspace((unsigned char)cleaned[i])) {
    i++;
  }
  if (cleaned[i] != '(') {
    arena_maybe_free(cleaned);
    return false;
  }
  i++;
  while (isspace((unsigned char)cleaned[i])) {
    i++;
  }
  if (cleaned[i] != ')') {
    arena_maybe_free(cleaned);
    return false;
  }
  i++;
  while (isspace((unsigned char)cleaned[i])) {
    i++;
  }
  result = cleaned[i] == '\0';
  arena_maybe_free(cleaned);
  return result;
}

static int execute_pipeline(struct shell_state *state, const char *source,
                            const struct program_text_hooks *hooks) {
  char *normalized;
  char *work;
  char *cursor;
  bool negate;
  size_t i;
  size_t start;
  char quote;
  int paren_depth;
  int brace_depth;
  int if_depth;
  int case_depth;
  int loop_depth;
  char **commands;
  size_t cmd_len;
  pid_t *pids;
  pid_t pipeline_pgid;
  bool isolate_pipeline_pgid;
  bool pipefail_snapshot;
  int *command_statuses;
  int *wait_statuses;
  bool *have_wait_statuses;
  int last_status;
  int in_fd;

  normalized = program_text_collapse_line_continuations(source);
  work = text_dup_trimmed_slice(normalized, 0, strlen(normalized));
  arena_maybe_free(normalized);
  cursor = work;
  negate = false;

  for (;;) {
    while (isspace((unsigned char)*cursor)) {
      cursor++;
    }

    if (*cursor != '!') {
      break;
    }
    if (cursor[1] != '\0' && !isspace((unsigned char)cursor[1]) &&
        cursor[1] != '(') {
      break;
    }

    negate = !negate;
    cursor++;
  }

  commands = NULL;
  cmd_len = 0;
  quote = '\0';
  paren_depth = 0;
  brace_depth = 0;
  if_depth = 0;
  case_depth = 0;
  loop_depth = 0;
  start = 0;

  for (i = 0;; i++) {
    char ch;
    bool delim;

    ch = cursor[i];
    delim = false;

    if (ch == '\0') {
      delim = true;
    } else if (quote == '\0') {
      if (ch == '\\' && cursor[i + 1] != '\0') {
        i++;
        continue;
      }
      if (paren_depth == 0 && brace_depth == 0 &&
          (isalpha((unsigned char)ch) || ch == '_') &&
          text_word_starts_command_position(cursor, i)) {
        size_t j;
        size_t boundary;
        char keyword[16];
        size_t kwlen;

        j = i;
        kwlen = 0;
        while (cursor[j] != '\0') {
          if (cursor[j] == '\\' && cursor[j + 1] == '\n') {
            j += 2;
            continue;
          }
          if (!isalnum((unsigned char)cursor[j]) && cursor[j] != '_') {
            break;
          }
          if (kwlen + 1 < sizeof(keyword)) {
            keyword[kwlen] = cursor[j];
          }
          kwlen++;
          j++;
        }
        boundary = text_skip_continuations_forward(cursor, j);
        if (text_keyword_boundary(cursor[boundary]) &&
            cursor[boundary] != ')') {
          if (kwlen == 2 && strncmp(keyword, "if", 2) == 0) {
            if_depth++;
          } else if (kwlen == 2 && strncmp(keyword, "fi", 2) == 0 &&
                     if_depth > 0) {
            if_depth--;
          } else if (kwlen == 4 && strncmp(keyword, "case", 4) == 0) {
            case_depth++;
          } else if (kwlen == 4 && strncmp(keyword, "esac", 4) == 0 &&
                     case_depth > 0) {
            case_depth--;
          } else if (case_depth == 0) {
            if ((kwlen == 5 && strncmp(keyword, "while", 5) == 0) ||
                (kwlen == 5 && strncmp(keyword, "until", 5) == 0) ||
                (kwlen == 3 && strncmp(keyword, "for", 3) == 0)) {
              loop_depth++;
            } else if (kwlen == 4 && strncmp(keyword, "done", 4) == 0 &&
                       loop_depth > 0 &&
                       keyword_preceded_by_list_separator(cursor, i)) {
              loop_depth--;
            }
          }
        }
        i = j - 1;
        continue;
      }
      if (ch == '\'' || ch == '"') {
        quote = ch;
      } else if (ch == '(') {
        paren_depth++;
      } else if (ch == ')' && paren_depth > 0) {
        paren_depth--;
      } else if (ch == '{') {
        brace_depth++;
      } else if (ch == '}' && brace_depth > 0) {
        brace_depth--;
      } else if (paren_depth == 0 && brace_depth == 0 && if_depth == 0 &&
                 case_depth == 0 && loop_depth == 0 && ch == '|' &&
                 cursor[i + 1] != '|' && !(i > 0 && cursor[i - 1] == '|') &&
                 !(i > 0 && cursor[i - 1] == '>')) {
        delim = true;
      }
    } else if (quote == '\'' && ch == '\'') {
      quote = '\0';
    } else if (quote == '"') {
      if (ch == '\\' && cursor[i + 1] != '\0') {
        i++;
        continue;
      }
      if (ch == '"') {
        quote = '\0';
      }
    }

    if (delim) {
      char *part;

      part = text_dup_trimmed_slice(cursor, start, i);
      if (part[0] != '\0') {
        commands = xrealloc(commands, sizeof(*commands) * (cmd_len + 1));
        commands[cmd_len++] = part;
      } else {
        arena_maybe_free(part);
      }

      if (ch == '\0') {
        break;
      }

      start = i + 1;
    }
  }

  if (cmd_len == 0) {
    arena_maybe_free(commands);
    arena_maybe_free(work);
    return 0;
  }

  if (cmd_len == 1) {
    int status;
    bool ignored;

    status = hooks->run_command_atom(state, commands[0], true);
    ignored = state->errexit_ignored;
    state->errexit_ignored = status != 0 && ignored;
    state->last_status = status;
    free_string_vec(commands, cmd_len);
    arena_maybe_free(work);
    if (negate) {
      state->errexit_ignored = true;
      state->last_status = status == 0 ? 1 : 0;
      return state->last_status;
    }
    return status;
  }

  pids = arena_xmalloc(sizeof(*pids) * cmd_len);
  command_statuses = arena_xmalloc(sizeof(*command_statuses) * cmd_len);
  wait_statuses = arena_xmalloc(sizeof(*wait_statuses) * cmd_len);
  have_wait_statuses = arena_xmalloc(sizeof(*have_wait_statuses) * cmd_len);
  memset(wait_statuses, 0, sizeof(*wait_statuses) * cmd_len);
  memset(have_wait_statuses, 0, sizeof(*have_wait_statuses) * cmd_len);
  pipeline_pgid = -1;
  isolate_pipeline_pgid = state->monitor_mode && state->main_context;
  pipefail_snapshot = state->pipefail;
  in_fd = -1;

  for (i = 0; i < cmd_len; i++) {
    int pipefd[2];
    pid_t pid;

    pipefd[0] = -1;
    pipefd[1] = -1;

    if (i + 1 < cmd_len) {
      if (pipe(pipefd) != 0) {
        perror("pipe");
        free_string_vec(commands, cmd_len);
        arena_maybe_free(pids);
        arena_maybe_free(command_statuses);
        arena_maybe_free(wait_statuses);
        arena_maybe_free(have_wait_statuses);
        arena_maybe_free(work);
        return 1;
      }
    }

    pid = fork();
    if (pid < 0) {
      perror("fork");
      if (pipefd[0] >= 0) {
        close(pipefd[0]);
        close(pipefd[1]);
      }
      free_string_vec(commands, cmd_len);
      arena_maybe_free(pids);
      arena_maybe_free(command_statuses);
      arena_maybe_free(wait_statuses);
      arena_maybe_free(have_wait_statuses);
      arena_maybe_free(work);
      return 1;
    }

    if (pid == 0) {
      if (isolate_pipeline_pgid) {
        pid_t target_pgid;

        target_pgid = (i == 0) ? 0 : pipeline_pgid;
        if (setpgid(0, target_pgid) != 0 && errno != EACCES && errno != ESRCH &&
            errno != EPERM && errno != EINVAL) {
          _exit(1);
        }
      }

      if (in_fd >= 0) {
        if (dup2(in_fd, STDIN_FILENO) < 0) {
          perror("dup2");
          _exit(1);
        }
      }
      if (pipefd[1] >= 0) {
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) {
          perror("dup2");
          _exit(1);
        }
      }

      if (in_fd >= 0) {
        close(in_fd);
      }
      if (pipefd[0] >= 0) {
        close(pipefd[0]);
      }
      if (pipefd[1] >= 0) {
        close(pipefd[1]);
      }

      hooks->exec_child_command(state, commands[i]);
    }

    pids[i] = pid;
    if (isolate_pipeline_pgid) {
      if (pipeline_pgid <= 0) {
        pipeline_pgid = pid;
      }
      if (setpgid(pid, pipeline_pgid) != 0 && errno != EACCES &&
          errno != ESRCH && errno != EPERM && errno != EINVAL) {
      }
    }

    if (in_fd >= 0) {
      close(in_fd);
    }
    if (pipefd[1] >= 0) {
      close(pipefd[1]);
    }
    in_fd = pipefd[0];
  }

  if (in_fd >= 0) {
    close(in_fd);
  }

  last_status = 0;
  for (i = 0; i < cmd_len; i++) {
    int wstatus;
    pid_t w;
    int command_status;

    for (;;) {
      w = waitpid(pids[i], &wstatus, WUNTRACED);
      if (w < 0 && errno == EINTR) {
        shell_run_pending_traps(state);
        continue;
      }
      break;
    }

    if (w < 0) {
      perror("waitpid");
      command_statuses[i] = 1;
      continue;
    }

    wait_statuses[i] = wstatus;
    have_wait_statuses[i] = true;

    if (WIFEXITED(wstatus)) {
      command_status = WEXITSTATUS(wstatus);
    } else if (WIFSTOPPED(wstatus)) {
      pid_t job_pgid;
      pid_t status_pid;
      size_t j;

      job_pgid =
          isolate_pipeline_pgid && pipeline_pgid > 0 ? pipeline_pgid : pids[i];
      status_pid = pids[cmd_len - 1];
      jobs_track_job(job_pgid, pids, cmd_len, status_pid, source, true);
      for (j = 0; j < cmd_len; j++) {
        if (have_wait_statuses[j]) {
          jobs_note_process_status(pids[j], wait_statuses[j]);
        }
      }
      command_status = shell_status_from_wait_status(wstatus);
    } else if (WIFSIGNALED(wstatus)) {
      command_status = shell_status_from_wait_status(wstatus);
    } else {
      command_status = 1;
    }
    if (!WIFSTOPPED(wstatus)) {
      struct jobs_entry_info tracked_job;

      if (jobs_find_by_pid(pids[i], &tracked_job)) {
        jobs_note_process_status(pids[i], wstatus);
      }
    }
    command_statuses[i] = command_status;
  }

  if (pipefail_snapshot) {
    int last_non_zero;

    last_non_zero = 0;
    for (i = 0; i < cmd_len; i++) {
      if (command_statuses[i] != 0) {
        last_non_zero = command_statuses[i];
      }
    }
    last_status = last_non_zero;
  } else {
    last_status = command_statuses[cmd_len - 1];
  }

  free_string_vec(commands, cmd_len);
  arena_maybe_free(pids);
  arena_maybe_free(command_statuses);
  arena_maybe_free(wait_statuses);
  arena_maybe_free(have_wait_statuses);
  arena_maybe_free(work);

  if (negate) {
    state->errexit_ignored = true;
    state->last_status = last_status == 0 ? 1 : 0;
    return state->last_status;
  }
  state->errexit_ignored = false;
  state->last_status = last_status;
  return last_status;
}

static int execute_andor(struct shell_state *state, const char *source,
                         const struct program_text_hooks *hooks) {
  char *normalized;
  size_t i;
  size_t start;
  char quote;
  int paren_depth;
  int brace_depth;
  int if_depth;
  int case_depth;
  int loop_depth;
  char **parts;
  enum andor_op *ops;
  size_t part_len;
  size_t op_len;
  int status;
  bool errexit_ignored;

  normalized = program_text_collapse_line_continuations(source);
  source = normalized;

  if (compound_needs_single_atom(source)) {
    status = execute_pipeline(state, source, hooks);
    arena_maybe_free(normalized);
    return status;
  }

  parts = NULL;
  ops = NULL;
  part_len = 0;
  op_len = 0;
  quote = '\0';
  paren_depth = 0;
  brace_depth = 0;
  if_depth = 0;
  case_depth = 0;
  loop_depth = 0;
  start = 0;
  errexit_ignored = false;

  for (i = 0;; i++) {
    char ch;
    bool delim;

    ch = source[i];
    delim = false;

    if (ch == '\0') {
      delim = true;
    } else if (quote == '\0') {
      if (ch == '\\' && source[i + 1] != '\0') {
        i++;
        continue;
      }
      if (ch == '$' && source[i + 1] == '(') {
        size_t end;

        if (find_command_subst_end(source, i, &end)) {
          i = end;
          continue;
        }
      }
      if (ch == '$' && source[i + 1] == '\'') {
        size_t end;

        if (find_dollar_single_quote_end(source, i, &end)) {
          i = end;
          continue;
        }
      }
      if (paren_depth == 0 && brace_depth == 0 &&
          (isalpha((unsigned char)ch) || ch == '_') &&
          text_word_starts_command_position(source, i)) {
        size_t j;
        size_t boundary;
        char keyword[16];
        size_t kwlen;

        j = i;
        kwlen = 0;
        while (source[j] != '\0') {
          if (source[j] == '\\' && source[j + 1] == '\n') {
            j += 2;
            continue;
          }
          if (!isalnum((unsigned char)source[j]) && source[j] != '_') {
            break;
          }
          if (kwlen + 1 < sizeof(keyword)) {
            keyword[kwlen] = source[j];
          }
          kwlen++;
          j++;
        }
        boundary = text_skip_continuations_forward(source, j);
        if (text_keyword_boundary(source[boundary]) &&
            source[boundary] != ')') {
          if (kwlen == 2 && strncmp(keyword, "if", 2) == 0) {
            if_depth++;
          } else if (kwlen == 2 && strncmp(keyword, "fi", 2) == 0 &&
                     if_depth > 0) {
            if_depth--;
          } else if (kwlen == 4 && strncmp(keyword, "case", 4) == 0) {
            case_depth++;
          } else if (kwlen == 4 && strncmp(keyword, "esac", 4) == 0 &&
                     case_depth > 0) {
            case_depth--;
          } else if (case_depth == 0) {
            if ((kwlen == 5 && strncmp(keyword, "while", 5) == 0) ||
                (kwlen == 5 && strncmp(keyword, "until", 5) == 0) ||
                (kwlen == 3 && strncmp(keyword, "for", 3) == 0)) {
              loop_depth++;
            } else if (kwlen == 4 && strncmp(keyword, "done", 4) == 0 &&
                       loop_depth > 0 &&
                       keyword_preceded_by_list_separator(source, i)) {
              loop_depth--;
            }
          }
        }
        i = j - 1;
        continue;
      }
      if (ch == '\'' || ch == '"') {
        quote = ch;
      } else if (ch == '(') {
        paren_depth++;
      } else if (ch == ')' && paren_depth > 0) {
        paren_depth--;
      } else if (ch == '{') {
        brace_depth++;
      } else if (ch == '}' && brace_depth > 0) {
        brace_depth--;
      } else if (paren_depth == 0 && brace_depth == 0 && if_depth == 0 &&
                 case_depth == 0 && loop_depth == 0 && ch == '&' &&
                 source[i + 1] == '&') {
        delim = true;
      } else if (paren_depth == 0 && brace_depth == 0 && if_depth == 0 &&
                 case_depth == 0 && loop_depth == 0 && ch == '|' &&
                 source[i + 1] == '|') {
        delim = true;
      }
    } else if (quote == '\'' && ch == '\'') {
      quote = '\0';
    } else if (quote == '"') {
      if (ch == '$' && source[i + 1] == '(') {
        size_t end;

        if (find_command_subst_end(source, i, &end)) {
          i = end;
          continue;
        }
      }
      if (ch == '\\' && source[i + 1] != '\0') {
        i++;
        continue;
      }
      if (ch == '"') {
        quote = '\0';
      }
    }

    if (delim) {
      char *part;

      part = text_dup_trimmed_slice(source, start, i);
      if (part[0] != '\0') {
        parts = xrealloc(parts, sizeof(*parts) * (part_len + 1));
        parts[part_len++] = part;
      } else {
        arena_maybe_free(part);
      }

      if (ch == '&') {
        ops = xrealloc(ops, sizeof(*ops) * (op_len + 1));
        ops[op_len++] = ANDOR_AND;
      } else if (ch == '|') {
        ops = xrealloc(ops, sizeof(*ops) * (op_len + 1));
        ops[op_len++] = ANDOR_OR;
      }

      i++;
      start = i + 1;
    }
  }

  if (part_len == 0) {
    arena_maybe_free(parts);
    arena_maybe_free(ops);
    arena_maybe_free(normalized);
    return 0;
  }

  status = execute_pipeline(state, parts[0], hooks);
  errexit_ignored = state->errexit_ignored;
  if (state->should_exit || hooks->has_pending_flow_control(state)) {
    free_string_vec(parts, part_len);
    arena_maybe_free(ops);
    arena_maybe_free(normalized);
    state->errexit_ignored = status != 0 && errexit_ignored;
    return status;
  }

  for (i = 0; i < op_len && i + 1 < part_len; i++) {
    if (ops[i] == ANDOR_AND) {
      if (status == 0) {
        status = execute_pipeline(state, parts[i + 1], hooks);
        errexit_ignored = state->errexit_ignored;
      } else {
        errexit_ignored = true;
      }
    } else {
      if (status != 0) {
        status = execute_pipeline(state, parts[i + 1], hooks);
        errexit_ignored = state->errexit_ignored;
      }
    }
    if (state->should_exit) {
      break;
    }
    if (hooks->has_pending_flow_control(state)) {
      break;
    }
  }

  free_string_vec(parts, part_len);
  arena_maybe_free(ops);
  arena_maybe_free(normalized);
  state->errexit_ignored = status != 0 && errexit_ignored;
  return status;
}

bool program_text_needs_more_input(const char *source, bool include_heredoc) {
  size_t len;

  if (source == NULL) {
    return false;
  }

  len = strlen(source);
  return shell_needs_more_input_text_mode(source, len, include_heredoc) != 0;
}

char *exec_alias_expand_preview(struct shell_state *state, const char *source) {
  char *logical;
  char *part;
  char *rewritten;
  bool changed;

  if (source == NULL) {
    return NULL;
  }

  logical = program_text_collapse_line_continuations(source);
  part = text_dup_trimmed_slice(logical, 0, strlen(logical));
  arena_maybe_free(logical);
  if (part[0] == '\0') {
    arena_maybe_free(part);
    return NULL;
  }

  rewritten = NULL;
  changed = false;
  if (alias_rewrite_snippet(state, part, &rewritten, &changed) != 0) {
    arena_maybe_free(part);
    return NULL;
  }
  if (changed) {
    arena_maybe_free(part);
    part = rewritten;
  } else {
    arena_maybe_free(rewritten);
    arena_maybe_free(part);
    return NULL;
  }

  if (part[0] == '\0') {
    arena_maybe_free(part);
    return NULL;
  }
  return part;
}

bool exec_alias_preview_needs_more(const char *preview) {
  if (preview == NULL || preview[0] == '\0') {
    return false;
  }
  return program_text_needs_more_input(preview, false);
}

int program_text_execute(struct shell_state *state, const char *source,
                         const struct program_text_hooks *hooks) {
  return program_text_execute_internal(state, source, true, hooks);
}

int program_text_execute_internal(struct shell_state *state,
                                  const char *source, bool apply_aliases,
                                  const struct program_text_hooks *hooks) {
  size_t i;
  size_t start;
  char quote;
  int paren_depth;
  int brace_depth;
  int if_depth;
  int case_depth;
  int loop_depth;
  int status;
  bool pending_heredoc;
  char *pending_function_head;
  char *pending_raw;
  size_t pending_start;
  size_t source_len;
  struct arena *saved_arena;
  struct arena_mark program_mark;
  bool have_program_mark;

  quote = '\0';
  paren_depth = 0;
  brace_depth = 0;
  if_depth = 0;
  case_depth = 0;
  loop_depth = 0;
  start = 0;
  status = 0;
  pending_heredoc = false;
  pending_function_head = NULL;
  pending_raw = NULL;
  pending_start = 0;
  source_len = strlen(source);
  saved_arena = arena_get_current();
  have_program_mark = saved_arena != NULL;
  if (have_program_mark) {
    arena_mark_take(saved_arena, &program_mark);
  }

  if (strstr(source, "<<") != NULL) {
    bool saved_suppress_aliases;

    saved_suppress_aliases = state->suppress_ast_aliases;
    if (!apply_aliases) {
      state->suppress_ast_aliases = true;
    }
    if (hooks->try_run_ast_compound_command(state, source, true, &status)) {
      state->suppress_ast_aliases = saved_suppress_aliases;
      if (have_program_mark) {
        arena_mark_rewind(saved_arena, &program_mark);
        arena_set_current(saved_arena);
      }
      return status;
    }
    state->suppress_ast_aliases = saved_suppress_aliases;
  }

  for (i = 0;; i++) {
    char ch;
    bool delim;

    ch = source[i];
    delim = false;

    if (ch == '\0') {
      delim = true;
    } else if (shell_position_in_comment(source, source_len, i)) {
      if (ch == '#') {
        size_t comment_end;

        comment_end = i;
        while (source[comment_end] != '\0' && source[comment_end] != '\n') {
          comment_end++;
        }
        if (comment_end > i) {
          i = comment_end - 1;
        }
      }
      continue;
    } else if (quote == '\0') {
      if (ch == '\\' && source[i + 1] != '\0') {
        i++;
        continue;
      }
      if (ch == '$' && source[i + 1] == '(') {
        size_t end;

        if (find_command_subst_end(source, i, &end)) {
          i = end;
          continue;
        }
      }
      if (ch == '$' && source[i + 1] == '\'') {
        size_t end;

        if (find_dollar_single_quote_end(source, i, &end)) {
          i = end;
          continue;
        }
      }
      if (ch == '\'' || ch == '"') {
        quote = ch;
      } else if (paren_depth == 0 && brace_depth == 0 &&
                 (isalpha((unsigned char)ch) || ch == '_') &&
                 text_word_starts_command_position(source, i)) {
        size_t j;
        size_t boundary;
        char keyword[16];
        size_t kwlen;

        j = i;
        kwlen = 0;
        while (source[j] != '\0') {
          if (source[j] == '\\' && source[j + 1] == '\n') {
            j += 2;
            continue;
          }
          if (!isalnum((unsigned char)source[j]) && source[j] != '_') {
            break;
          }
          if (kwlen + 1 < sizeof(keyword)) {
            keyword[kwlen] = source[j];
          }
          kwlen++;
          j++;
        }
        boundary = text_skip_continuations_forward(source, j);
        if (text_keyword_boundary(source[boundary]) &&
            source[boundary] != ')') {
          if (kwlen == 2 && strncmp(keyword, "if", 2) == 0) {
            if_depth++;
          } else if (kwlen == 2 && strncmp(keyword, "fi", 2) == 0 &&
                     if_depth > 0) {
            if_depth--;
          } else if (kwlen == 4 && strncmp(keyword, "case", 4) == 0) {
            case_depth++;
          } else if (kwlen == 4 && strncmp(keyword, "esac", 4) == 0 &&
                     case_depth > 0) {
            case_depth--;
          } else if (case_depth == 0) {
            if ((kwlen == 5 && strncmp(keyword, "while", 5) == 0) ||
                (kwlen == 5 && strncmp(keyword, "until", 5) == 0) ||
                (kwlen == 3 && strncmp(keyword, "for", 3) == 0)) {
              loop_depth++;
            } else if (kwlen == 4 && strncmp(keyword, "done", 4) == 0 &&
                       loop_depth > 0 &&
                       keyword_preceded_by_list_separator(source, i)) {
              loop_depth--;
            }
          }
        }
        i = j - 1;
        continue;
      } else if (ch == '(') {
        paren_depth++;
      } else if (ch == ')' && paren_depth > 0) {
        paren_depth--;
      } else if (ch == '{') {
        brace_depth++;
      } else if (ch == '}' && brace_depth > 0) {
        brace_depth--;
      } else if (paren_depth == 0 && brace_depth == 0 && ch == '<' &&
                 source[i + 1] == '<') {
        pending_heredoc = true;
      } else if (paren_depth == 0 && brace_depth == 0 && if_depth == 0 &&
                 case_depth == 0 && loop_depth == 0 &&
                 ((ch == ';' && !pending_heredoc) ||
                  (ch == '\n' && !pending_heredoc &&
                   !newline_continues_command(source, source_len, i)) ||
                  (is_async_separator_amp(source, i) && !pending_heredoc))) {
        delim = true;
      }
    } else if (quote == '\'' && ch == '\'') {
      quote = '\0';
    } else if (quote == '"') {
      if (ch == '$' && source[i + 1] == '(') {
        size_t end;

        if (find_command_subst_end(source, i, &end)) {
          i = end;
          continue;
        }
      }
      if (ch == '\\' && source[i + 1] != '\0') {
        i++;
        continue;
      }
      if (ch == '"') {
        quote = '\0';
      }
    }

    if (delim) {
      char *chunk_raw;
      char *raw_part;
      char *part;
      char *logical_part;
      size_t command_start;
      bool heredoc_chunk;

      chunk_raw = text_dup_slice(source, start, i);
      if (pending_raw != NULL) {
        size_t pending_len;
        size_t chunk_len;

        pending_len = strlen(pending_raw);
        chunk_len = strlen(chunk_raw);
        raw_part = arena_xmalloc(pending_len + chunk_len + 1);
        memcpy(raw_part, pending_raw, pending_len);
        memcpy(raw_part + pending_len, chunk_raw, chunk_len + 1);
        command_start = pending_start;
        arena_maybe_free(pending_raw);
        pending_raw = NULL;
        arena_maybe_free(chunk_raw);
      } else {
        raw_part = chunk_raw;
        command_start = start;
      }

      logical_part = program_text_collapse_line_continuations(raw_part);
      {
        char *comment_stripped_part;

        comment_stripped_part = strip_comments(logical_part);
        part = text_dup_trimmed_slice(comment_stripped_part, 0,
                                 strlen(comment_stripped_part));
        arena_maybe_free(comment_stripped_part);
      }
      arena_maybe_free(logical_part);
      heredoc_chunk = pending_heredoc;
      if (part[0] != '\0') {
        bool alias_changed;

        alias_changed = false;
        if (apply_aliases) {
          char *alias_rewritten_part;

          alias_rewritten_part = NULL;
          if (alias_rewrite_snippet(state, part, &alias_rewritten_part,
                                    &alias_changed) != 0) {
            arena_maybe_free(part);
            arena_maybe_free(raw_part);
            status = 2;
            break;
          }
          if (alias_changed) {
            arena_maybe_free(part);
            part = alias_rewritten_part;
          } else {
            arena_maybe_free(alias_rewritten_part);
          }

          if (part[0] == '\0') {
            status = state->last_status;
            arena_maybe_free(part);
            arena_maybe_free(raw_part);
            if (ch == '\0') {
              break;
            }
            start = i + 1;
            pending_heredoc = false;
            continue;
          }

          if (alias_changed && ch != '\0' &&
              program_text_needs_more_input(part,
                                            strstr(part, "<<") != NULL)) {
            size_t raw_len;

            raw_len = strlen(raw_part);
            pending_raw = arena_xmalloc(raw_len + 2);
            memcpy(pending_raw, raw_part, raw_len);
            pending_raw[raw_len] = ch;
            pending_raw[raw_len + 1] = '\0';
            pending_start = command_start;
            arena_maybe_free(part);
            arena_maybe_free(raw_part);
            start = i + 1;
            pending_heredoc = false;
            continue;
          }
        }

        if (pending_function_head != NULL) {
          size_t hlen;
          size_t plen;
          char *combined;

          hlen = strlen(pending_function_head);
          plen = strlen(part);
          combined = arena_xmalloc(hlen + 1 + plen + 1);
          memcpy(combined, pending_function_head, hlen);
          combined[hlen] = '\n';
          memcpy(combined + hlen + 1, part, plen + 1);
          arena_maybe_free(pending_function_head);
          arena_maybe_free(part);
          pending_function_head = NULL;
          part = combined;
        }

        if (looks_like_function_header_only(part) && ch != '\0') {
          pending_function_head = part;
          arena_maybe_free(raw_part);
          start = i + 1;
          pending_heredoc = false;
          continue;
        }

        set_lineno_for_command(state, source, command_start);
        state->errexit_ignored = false;
        if (ch == '&') {
          status = hooks->run_async_list(state, part);
          state->last_status = status;
        } else if (heredoc_chunk) {
          bool saved_suppress_aliases;

          saved_suppress_aliases = state->suppress_ast_aliases;
          if (alias_changed) {
            state->suppress_ast_aliases = true;
          }
          if (!hooks->try_run_ast_compound_command(state, part, true, &status)) {
            status = 2;
          }
          state->suppress_ast_aliases = saved_suppress_aliases;
          state->last_status = status;
        } else {
          if (apply_aliases && alias_changed &&
              command_requires_program_runner(part)) {
            char *alias_cleaned;
            bool saved_suppress_aliases;

            alias_cleaned = strip_comments(part);
            saved_suppress_aliases = state->suppress_ast_aliases;
            state->suppress_ast_aliases = true;
            if (!hooks->try_run_ast_compound_command(state, alias_cleaned, true,
                                                     &status)) {
              status = program_text_execute_internal(state, alias_cleaned, false,
                                                     hooks);
            }
            state->suppress_ast_aliases = saved_suppress_aliases;
            arena_maybe_free(alias_cleaned);
          } else {
            bool saved_suppress_aliases;

            saved_suppress_aliases = state->suppress_ast_aliases;
            if (alias_changed) {
              state->suppress_ast_aliases = true;
            }
            if (!hooks->try_run_ast_compound_command(state, part, true,
                                                     &status)) {
              status = execute_andor(state, part, hooks);
            }
            state->suppress_ast_aliases = saved_suppress_aliases;
          }
          state->last_status = status;
          if (status != 0 && state->errexit && !state->interactive &&
              !state->errexit_ignored) {
            state->should_exit = true;
            state->exit_status = status;
          }
        }
        shell_run_pending_traps(state);
        if (state->should_exit) {
          arena_maybe_free(part);
          arena_maybe_free(raw_part);
          break;
        }
        if (hooks->has_pending_flow_control(state)) {
          arena_maybe_free(part);
          arena_maybe_free(raw_part);
          break;
        }
      }
      arena_maybe_free(part);
      arena_maybe_free(raw_part);

      if (ch == '\0') {
        break;
      }

      start = i + 1;
      pending_heredoc = false;
    }
  }

  if (pending_function_head != NULL) {
    status = execute_andor(state, pending_function_head, hooks);
    arena_maybe_free(pending_function_head);
  }
  arena_maybe_free(pending_raw);
  if (have_program_mark) {
    arena_mark_rewind(saved_arena, &program_mark);
    arena_set_current(saved_arena);
  }

  return status;
}
