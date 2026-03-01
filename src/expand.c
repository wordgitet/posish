/* SPDX-License-Identifier: 0BSD */

/* posish - expansion */

#include "expand.h"

#include "arena.h"
#include "arith.h"
#include "error.h"
#include "signals.h"
#include "vars.h"

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void *xrealloc(void *ptr, size_t size) {
  void *p;

  p = realloc(ptr, size);
  if (p == NULL) {
    perror("realloc");
    exit(EXIT_FAILURE);
  }
  return p;
}

static void append_char(char **buf, size_t *len, size_t *cap, char ch) {
  if (*len + 2 > *cap) {
    size_t new_cap;

    new_cap = *cap == 0 ? 32 : *cap;
    while (*len + 2 > new_cap) {
      new_cap *= 2;
    }
    *buf = xrealloc(*buf, new_cap);
    *cap = new_cap;
  }

  (*buf)[(*len)++] = ch;
  (*buf)[*len] = '\0';
}

static void append_str(char **buf, size_t *len, size_t *cap, const char *str) {
  size_t i;

  for (i = 0; str[i] != '\0'; i++) {
    append_char(buf, len, cap, str[i]);
  }
}

static int expand_token(const char *in, struct shell_state *state, char **out);

static bool token_is_unquoted(const char *token) {
  size_t i;

  i = 0;
  while (token[i] != '\0') {
    if (token[i] == '\\' && token[i + 1] != '\0') {
      i += 2;
      continue;
    }
    if (token[i] == '\'' || token[i] == '"') {
      return false;
    }
    i++;
  }
  return true;
}

static bool is_short_parameter_char(char ch) {
  return ch == '$' || ch == '?' || ch == '!' || ch == '#' || ch == '@' ||
         ch == '*' || ch == '-' || isdigit((unsigned char)ch);
}

static bool inherited_ignore_locked(const struct shell_state *state,
                                    int signo) {
  return !state->interactive && signals_inherited_ignored(signo) &&
         !state->parent_was_interactive;
}

static void reset_signal_traps_for_cmdsub(struct shell_state *state) {
  int signo;

  for (signo = 1; signo < NSIG; signo++) {
    if (state->signal_traps[signo] != NULL) {
      if (state->signal_traps[signo][0] == '\0') {
        (void)signals_set_ignored(signo);
      } else {
        state->signal_traps[signo] = NULL;
        if (inherited_ignore_locked(state, signo)) {
          (void)signals_set_ignored(signo);
        } else {
          (void)signals_set_default(signo);
        }
      }
    } else if (state->signal_cleared[signo]) {
      (void)signals_set_default(signo);
    } else {
      struct sigaction sa;

      if (sigaction(signo, NULL, &sa) == 0 && sa.sa_handler == SIG_IGN &&
          signals_policy_ignored(signo) && !signals_inherited_ignored(signo)) {
        (void)signals_set_default(signo);
      }
    }
    signals_clear_pending(signo);
  }
}

static int run_command_substitution(struct shell_state *state, const char *cmd,
                                    char **out_value, int *status_out) {
  int pipefd[2];
  pid_t pid;
  int status;
  char *buf;
  size_t len;
  size_t cap;

  if (pipe(pipefd) != 0) {
    perror("pipe");
    return -1;
  }

  pid = fork();
  if (pid < 0) {
    perror("fork");
    close(pipefd[0]);
    close(pipefd[1]);
    return -1;
  }

  if (pid == 0) {
    struct shell_state local_state;
    int st;

    dup2(pipefd[1], STDOUT_FILENO);
    close(pipefd[0]);
    close(pipefd[1]);

    local_state = *state;
    local_state.should_exit = false;
    local_state.exit_status = 0;
    local_state.running_signal_trap = false;
    local_state.main_context = false;
    reset_signal_traps_for_cmdsub(&local_state);

    st = shell_run_command(&local_state, cmd);
    if (local_state.should_exit) {
      st = local_state.exit_status;
    }
    fflush(NULL);
    _exit(st);
  }

  close(pipefd[1]);

  buf = arena_xmalloc(64);
  len = 0;
  cap = 64;

  for (;;) {
    ssize_t n;

    if (len + 64 > cap) {
      cap *= 2;
      buf = xrealloc(buf, cap);
    }

    n = read(pipefd[0], buf + len, cap - len - 1);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      perror("read");
      close(pipefd[0]);
      free(buf);
      return -1;
    }
    if (n == 0) {
      break;
    }

    len += (size_t)n;
  }

  close(pipefd[0]);

  for (;;) {
    if (waitpid(pid, &status, 0) < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    break;
  }

  while (len > 0 && buf[len - 1] == '\n') {
    len--;
  }

  buf[len] = '\0';
  if (WIFEXITED(status)) {
    *status_out = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    *status_out = 128 + WTERMSIG(status);
  } else {
    *status_out = 1;
  }
  *out_value = buf;
  return 0;
}

static int run_arithmetic_expansion(const char *expr, char **out_value,
                                    int *status_out,
                                    struct shell_state *state) {
  char *expanded_expr;
  long value;
  char text[64];

  if (expand_token(expr, state, &expanded_expr) != 0) {
    return -1;
  }

  if (arith_eval(expanded_expr, state, &value) != 0) {
    free(expanded_expr);
    return -1;
  }

  snprintf(text, sizeof(text), "%ld", value);
  *out_value = arena_xstrdup(text);
  *status_out = state->cmdsub_performed ? state->last_cmdsub_status : 0;
  free(expanded_expr);
  return 0;
}

static int append_parameter(const char *name, size_t nlen,
                            struct shell_state *state, char **buf, size_t *len,
                            size_t *cap) {
  char *tmp;
  const char *val;

  tmp = arena_xmalloc(nlen + 1);
  memcpy(tmp, name, nlen);
  tmp[nlen] = '\0';

  if (strcmp(tmp, "?") == 0) {
    char num[32];
    snprintf(num, sizeof(num), "%d", state->last_status);
    append_str(buf, len, cap, num);
    free(tmp);
    return 0;
  }

  if (strcmp(tmp, "$") == 0) {
    char num[32];
    snprintf(num, sizeof(num), "%ld", (long)state->shell_pid);
    append_str(buf, len, cap, num);
    free(tmp);
    return 0;
  }

  if (strcmp(tmp, "!") == 0) {
    if (state->last_async_pid > 0) {
      char num[32];

      snprintf(num, sizeof(num), "%ld", (long)state->last_async_pid);
      append_str(buf, len, cap, num);
    }
    free(tmp);
    return 0;
  }

  if (strcmp(tmp, "#") == 0) {
    char num[32];
    snprintf(num, sizeof(num), "%zu", state->positional_count);
    append_str(buf, len, cap, num);
    free(tmp);
    return 0;
  }

  if (strcmp(tmp, "*") == 0) {
    size_t i;
    char sep;

    sep = ' ';
    val = getenv("IFS");
    if (val != NULL && val[0] != '\0') {
      sep = val[0];
    }

    for (i = 0; i < state->positional_count; i++) {
      if (i > 0) {
        append_char(buf, len, cap, sep);
      }
      append_str(buf, len, cap, state->positional_params[i]);
    }
    free(tmp);
    return 0;
  }

  if (isdigit((unsigned char)tmp[0])) {
    char *end;
    unsigned long n;

    n = strtoul(tmp, &end, 10);
    if (end != tmp && *end == '\0' && n > 0 && n <= state->positional_count) {
      append_str(buf, len, cap, state->positional_params[n - 1]);
    }
    free(tmp);
    return 0;
  }

  val = getenv(tmp);
  if (val != NULL) {
    append_str(buf, len, cap, val);
  }

  free(tmp);
  return 0;
}

static int append_braced_parameter(const char *expr, size_t elen,
                                   struct shell_state *state, char **buf,
                                   size_t *len, size_t *cap) {
  size_t i;

  for (i = 0; i + 1 < elen; i++) {
    if (expr[i] == ':' && expr[i + 1] == '+') {
      size_t name_len;
      char *name;
      const char *value;
      bool set_nonempty;
      char *word;
      char *expanded_word;

      name_len = i;
      name = arena_xmalloc(name_len + 1);
      memcpy(name, expr, name_len);
      name[name_len] = '\0';

      value = getenv(name);
      set_nonempty = value != NULL && value[0] != '\0';
      if (set_nonempty) {
        size_t wlen;

        wlen = elen - (i + 2);
        word = arena_xmalloc(wlen + 1);
        memcpy(word, expr + i + 2, wlen);
        word[wlen] = '\0';

        if (expand_token(word, state, &expanded_word) != 0) {
          free(word);
          free(name);
          return -1;
        }
        append_str(buf, len, cap, expanded_word);
        free(expanded_word);
        free(word);
      }
      free(name);
      return 0;
    }
  }

  for (i = 0; i < elen; i++) {
    if (expr[i] == '=') {
      size_t name_len;
      char *name;
      const char *value;
      size_t wlen;
      char *word;
      char *expanded_word;

      name_len = i;
      name = arena_xmalloc(name_len + 1);
      memcpy(name, expr, name_len);
      name[name_len] = '\0';

      if (!vars_is_name_valid(name)) {
        free(name);
        break;
      }

      value = getenv(name);
      if (value == NULL) {
        wlen = elen - (i + 1);
        word = arena_xmalloc(wlen + 1);
        memcpy(word, expr + i + 1, wlen);
        word[wlen] = '\0';

        if (expand_token(word, state, &expanded_word) != 0) {
          free(word);
          free(name);
          return -1;
        }
        if (vars_set(state, name, expanded_word, true) != 0) {
          free(expanded_word);
          free(word);
          free(name);
          return -1;
        }
        free(expanded_word);
        free(word);
        value = getenv(name);
      }

      if (value != NULL) {
        append_str(buf, len, cap, value);
      }
      free(name);
      return 0;
    }
  }

  for (i = 0; i < elen; i++) {
    if (expr[i] == '%') {
      size_t name_len;
      char *name;
      const char *value;
      size_t plen;
      char *pattern;
      char *expanded_pattern;
      size_t vlen;
      size_t eplen;

      name_len = i;
      name = arena_xmalloc(name_len + 1);
      memcpy(name, expr, name_len);
      name[name_len] = '\0';
      value = getenv(name);
      if (value == NULL) {
        value = "";
      }

      plen = elen - (i + 1);
      pattern = arena_xmalloc(plen + 1);
      memcpy(pattern, expr + i + 1, plen);
      pattern[plen] = '\0';

      if (expand_token(pattern, state, &expanded_pattern) != 0) {
        free(pattern);
        free(name);
        return -1;
      }

      vlen = strlen(value);
      eplen = strlen(expanded_pattern);
      if (eplen > 0 && vlen >= eplen &&
          strcmp(value + (vlen - eplen), expanded_pattern) == 0) {
        char *trimmed;

        trimmed = arena_xmalloc(vlen - eplen + 1);
        memcpy(trimmed, value, vlen - eplen);
        trimmed[vlen - eplen] = '\0';
        append_str(buf, len, cap, trimmed);
        free(trimmed);
      } else {
        append_str(buf, len, cap, value);
      }

      free(expanded_pattern);
      free(pattern);
      free(name);
      return 0;
    }
  }

  return append_parameter(expr, elen, state, buf, len, cap);
}

static int expand_token(const char *in, struct shell_state *state, char **out) {
  size_t i;
  char *buf;
  size_t len;
  size_t cap;
  char quote;

  i = 0;
  buf = NULL;
  len = 0;
  cap = 0;
  quote = '\0';

  while (in[i] != '\0') {
    if (quote == '\'') {
      if (in[i] == '\'') {
        quote = '\0';
        i++;
        continue;
      }
      append_char(&buf, &len, &cap, in[i]);
      i++;
      continue;
    }

    if (quote == '"') {
      if (in[i] == '"') {
        quote = '\0';
        i++;
        continue;
      }
      if (in[i] == '\\' && in[i + 1] != '\0' &&
          (in[i + 1] == '$' || in[i + 1] == '`' || in[i + 1] == '"' ||
           in[i + 1] == '\\' || in[i + 1] == '\n')) {
        i++;
        if (in[i] == '\n') {
          i++;
          continue;
        }
        append_char(&buf, &len, &cap, in[i]);
        i++;
        continue;
      }
    } else {
      if (in[i] == '\'') {
        quote = '\'';
        i++;
        continue;
      }
      if (in[i] == '"') {
        quote = '"';
        i++;
        continue;
      }
      if (in[i] == '\\') {
        i++;
        if (in[i] == '\0') {
          posish_errorf("trailing backslash in expansion");
          free(buf);
          return -1;
        }
        if (in[i] == '\n') {
          i++;
          continue;
        }
        append_char(&buf, &len, &cap, in[i]);
        i++;
        continue;
      }
    }

    if (in[i] == '$') {
      if (in[i + 1] == '{') {
        size_t j;

        j = i + 2;
        while (in[j] != '\0' && in[j] != '}') {
          j++;
        }
        if (in[j] != '}') {
          posish_errorf("unterminated parameter expansion");
          free(buf);
          return -1;
        }

        if (j > i + 2) {
          if (append_braced_parameter(in + i + 2, j - (i + 2), state, &buf,
                                      &len, &cap) != 0) {
            free(buf);
            return -1;
          }
        }
        i = j + 1;
        continue;
      }

      if (in[i + 1] == '(') {
        if (in[i + 2] == '(') {
          size_t j;
          int depth;
          char *expr;
          char *value;
          int cmd_status;

          j = i + 3;
          depth = 0;
          while (in[j] != '\0') {
            if (in[j] == '\\' && in[j + 1] != '\0') {
              j += 2;
              continue;
            }
            if (in[j] == '(') {
              depth++;
              j++;
              continue;
            }
            if (in[j] == ')') {
              if (depth > 0) {
                depth--;
                j++;
                continue;
              }
              if (in[j + 1] == ')') {
                break;
              }
            }
            j++;
          }
          if (in[j] == '\0' || in[j + 1] != ')') {
            posish_errorf("unterminated arithmetic expansion");
            free(buf);
            return -1;
          }

          expr = arena_xmalloc((j - (i + 3)) + 1);
          memcpy(expr, in + i + 3, j - (i + 3));
          expr[j - (i + 3)] = '\0';

          if (run_arithmetic_expansion(expr, &value, &cmd_status, state) != 0) {
            free(expr);
            free(buf);
            return -1;
          }
          append_str(&buf, &len, &cap, value);
          state->last_cmdsub_status = cmd_status;
          state->cmdsub_performed = true;
          free(value);
          free(expr);
          i = j + 2;
          continue;
        }

        size_t j;
        int depth;
        char quote;
        char *cmd;
        char *value;
        int cmd_status;

        j = i + 2;
        depth = 1;
        quote = '\0';
        while (in[j] != '\0' && depth > 0) {
          char ch;

          ch = in[j];
          if (quote == '\0') {
            if (ch == '\\' && in[j + 1] != '\0') {
              j += 2;
              continue;
            }
            if (ch == '\'' || ch == '"') {
              quote = ch;
              j++;
              continue;
            }
            if (ch == '(') {
              depth++;
            } else if (ch == ')') {
              depth--;
              if (depth == 0) {
                break;
              }
            }
          } else if (quote == '\'' && ch == '\'') {
            quote = '\0';
          } else if (quote == '"') {
            if (ch == '\\' && in[j + 1] != '\0') {
              j += 2;
              continue;
            }
            if (ch == '"') {
              quote = '\0';
            }
          }
          j++;
        }

        if (in[j] != ')' || depth != 0) {
          posish_errorf("unterminated command substitution");
          free(buf);
          return -1;
        }

        cmd = arena_xmalloc((j - (i + 2)) + 1);
        memcpy(cmd, in + i + 2, j - (i + 2));
        cmd[j - (i + 2)] = '\0';

        if (run_command_substitution(state, cmd, &value, &cmd_status) != 0) {
          free(cmd);
          free(buf);
          return -1;
        }

        append_str(&buf, &len, &cap, value);
        state->last_cmdsub_status = cmd_status;
        state->cmdsub_performed = true;
        free(value);
        free(cmd);
        i = j + 1;
        continue;
      }

      if (in[i + 1] == '`') {
        append_char(&buf, &len, &cap, '$');
        i++;
        continue;
      }

      if (is_short_parameter_char(in[i + 1])) {
        append_parameter(in + i + 1, 1, state, &buf, &len, &cap);
        i += 2;
        continue;
      }

      if (isalpha((unsigned char)in[i + 1]) || in[i + 1] == '_') {
        size_t j;

        j = i + 1;
        while (isalnum((unsigned char)in[j]) || in[j] == '_') {
          j++;
        }
        append_parameter(in + i + 1, j - (i + 1), state, &buf, &len, &cap);
        i = j;
        continue;
      }
    }

    if (in[i] == '`') {
      size_t j;
      char *cmd;
      char *value;
      int cmd_status;

      j = i + 1;
      while (in[j] != '\0') {
        if (in[j] == '\\' && in[j + 1] != '\0') {
          j += 2;
          continue;
        }
        if (in[j] == '`') {
          break;
        }
        j++;
      }

      if (in[j] != '`') {
        posish_errorf("unterminated backtick command substitution");
        free(buf);
        return -1;
      }

      cmd = arena_xmalloc((j - (i + 1)) + 1);
      memcpy(cmd, in + i + 1, j - (i + 1));
      cmd[j - (i + 1)] = '\0';

      if (run_command_substitution(state, cmd, &value, &cmd_status) != 0) {
        free(cmd);
        free(buf);
        return -1;
      }

      append_str(&buf, &len, &cap, value);
      state->last_cmdsub_status = cmd_status;
      state->cmdsub_performed = true;
      free(value);
      free(cmd);
      i = j + 1;
      continue;
    }

    append_char(&buf, &len, &cap, in[i]);
    i++;
  }

  if (quote != '\0') {
    posish_errorf("unterminated quote in expansion");
    free(buf);
    return -1;
  }

  if (buf == NULL) {
    buf = arena_xstrdup("");
  }

  *out = buf;
  return 0;
}

int expand_words(const struct token_vec *in, struct token_vec *out,
                 struct shell_state *state, bool split_fields) {
  size_t i;

  out->items = NULL;
  out->len = 0;
  state->last_cmdsub_status = 0;
  state->cmdsub_performed = false;

  for (i = 0; i < in->len; i++) {
    char *expanded;

    /*
     * Preserve positional argument cardinality for "$@" and "\"$@\"" tokens.
     * The quoted form is heavily used by test harness helper functions.
     */
    if (strcmp(in->items[i], "$@") == 0 ||
        strcmp(in->items[i], "\"$@\"") == 0) {
      size_t j;
      for (j = 0; j < state->positional_count; j++) {
        out->items = xrealloc(out->items, sizeof(*out->items) * (out->len + 1));
        out->items[out->len++] = arena_xstrdup(state->positional_params[j]);
      }
      continue;
    }

    if (expand_token(in->items[i], state, &expanded) != 0) {
      size_t j;
      for (j = 0; j < out->len; j++) {
        free(out->items[j]);
      }
      free(out->items);
      out->items = NULL;
      out->len = 0;
      return -1;
    }

    /*
     * Unquoted empty expansions are removed from command words.
     * Quoted empties ('' or "") must be preserved.
     */
    if (split_fields && expanded[0] == '\0' &&
        token_is_unquoted(in->items[i])) {
      free(expanded);
      continue;
    }

    out->items = xrealloc(out->items, sizeof(*out->items) * (out->len + 1));
    out->items[out->len++] = expanded;
  }

  return 0;
}
