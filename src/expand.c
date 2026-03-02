/* SPDX-License-Identifier: 0BSD */

/* posish - expansion */

#include "expand.h"

#include "arena.h"
#include "arith.h"
#include "error.h"
#include "options.h"
#include "shell.h"
#include "signals.h"
#include "vars.h"

#include <ctype.h>
#include <errno.h>
#include <fnmatch.h>
#include <glob.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define QUOTED_IFS_SPACE '\x81'
#define QUOTED_IFS_TAB '\x82'
#define QUOTED_IFS_NEWLINE '\x83'
#define QUOTED_GLOB_STAR '\x84'
#define QUOTED_GLOB_QMARK '\x85'
#define QUOTED_GLOB_LBRACK '\x86'
#define QUOTED_EMPTY_MARK '\x87'
#define QUOTED_LITERAL_PREFIX '\x88'
#define PARAM_AT_SPLIT '\x89'
#define PATTERN_LIT_STAR '\x12'
#define PATTERN_LIT_QMARK '\x13'
#define PATTERN_LIT_LBRACK '\x14'
#define PATTERN_LIT_RBRACK '\x15'
#define PATTERN_LIT_BSLASH '\x16'

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

static char *collapse_line_continuations_copy(const char *in, size_t in_len,
                                              size_t *out_len) {
  size_t i;
  size_t j;
  char *out;

  out = arena_xmalloc(in_len + 1);
  i = 0;
  j = 0;
  while (i < in_len) {
    if (in[i] == '\\' && i + 1 < in_len && in[i + 1] == '\n') {
      i += 2;
      continue;
    }
    out[j++] = in[i++];
  }
  out[j] = '\0';
  *out_len = j;
  return out;
}

static int hex_digit_value(char ch) {
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }
  if (ch >= 'a' && ch <= 'f') {
    return 10 + (ch - 'a');
  }
  if (ch >= 'A' && ch <= 'F') {
    return 10 + (ch - 'A');
  }
  return -1;
}

static void append_tilde_literal_char(char **buf, size_t *len, size_t *cap,
                                      char ch);

static int append_dollar_single_quoted(const char *in, size_t *index, char **buf,
                                       size_t *len, size_t *cap) {
  size_t i;
  bool any_output;

#define APPEND_DSQ_CHAR(c_)                                                     \
  do {                                                                          \
    append_tilde_literal_char(buf, len, cap, (c_));                            \
    any_output = true;                                                          \
  } while (0)

  i = *index + 2;
  any_output = false;
  while (in[i] != '\0') {
    char ch;

    ch = in[i];
    if (ch == '\'') {
      if (!any_output) {
        append_char(buf, len, cap, QUOTED_EMPTY_MARK);
      }
      *index = i + 1;
      return 0;
    }
    if (ch != '\\') {
      APPEND_DSQ_CHAR(ch);
      i++;
      continue;
    }

    i++;
    ch = in[i];
    if (ch == '\0') {
      break;
    }
    if (ch == '\n') {
      i++;
      continue;
    }
    if (ch >= '0' && ch <= '7') {
      unsigned int value;
      int digits;

      value = (unsigned int)(ch - '0');
      i++;
      digits = 1;
      while (digits < 3 && in[i] >= '0' && in[i] <= '7') {
        value = value * 8u + (unsigned int)(in[i] - '0');
        i++;
        digits++;
      }
      APPEND_DSQ_CHAR((char)(value & 0xffu));
      continue;
    }
    if (ch == 'x') {
      unsigned int value;
      int v;
      bool have_hex;

      i++;
      value = 0;
      have_hex = false;
      while ((v = hex_digit_value(in[i])) >= 0) {
        value = value * 16u + (unsigned int)v;
        have_hex = true;
        i++;
      }
      if (!have_hex) {
        APPEND_DSQ_CHAR('x');
      } else {
        APPEND_DSQ_CHAR((char)(value & 0xffu));
      }
      continue;
    }
    if (ch == 'c') {
      unsigned char control;

      i++;
      if (in[i] == '\0') {
        APPEND_DSQ_CHAR('c');
        break;
      }
      if (in[i] == '\\' && in[i + 1] != '\0') {
        control = (unsigned char)in[i + 1];
        i += 2;
      } else {
        control = (unsigned char)in[i];
        i++;
      }
      if (control == '?') {
        APPEND_DSQ_CHAR((char)0x7f);
      } else {
        if (islower(control)) {
          control = (unsigned char)toupper(control);
        }
        APPEND_DSQ_CHAR((char)(control & 0x1f));
      }
      continue;
    }

    switch (ch) {
    case 'a':
      APPEND_DSQ_CHAR('\a');
      break;
    case 'b':
      APPEND_DSQ_CHAR('\b');
      break;
    case 'e':
      APPEND_DSQ_CHAR(0x1b);
      break;
    case 'f':
      APPEND_DSQ_CHAR('\f');
      break;
    case 'n':
      APPEND_DSQ_CHAR('\n');
      break;
    case 'r':
      APPEND_DSQ_CHAR('\r');
      break;
    case 't':
      APPEND_DSQ_CHAR('\t');
      break;
    case 'v':
      APPEND_DSQ_CHAR('\v');
      break;
    default:
      APPEND_DSQ_CHAR(ch);
      break;
    }
    i++;
  }

#undef APPEND_DSQ_CHAR

  posish_errorf("unterminated dollar-single-quoted string");
  return -1;
}

static size_t skip_token_line_continuations(const char *in, size_t pos) {
  while (in[pos] == '\\' && in[pos + 1] == '\n') {
    pos += 2;
  }
  return pos;
}

static int expand_token(const char *in, struct shell_state *state, char **out,
                        bool dquote_mode, bool assignment_context);

static bool is_name_start_char_local(char ch) {
  return isalpha((unsigned char)ch) || ch == '_';
}

static bool is_name_char_local(char ch) {
  return isalnum((unsigned char)ch) || ch == '_';
}

static bool assignment_value_start(const char *token, size_t *value_start) {
  size_t i;

  if (token[0] == '\0' || !is_name_start_char_local(token[0])) {
    return false;
  }

  i = 1;
  while (token[i] != '\0' && token[i] != '=') {
    if (!is_name_char_local(token[i])) {
      return false;
    }
    i++;
  }
  if (token[i] != '=') {
    return false;
  }

  *value_start = i + 1;
  return true;
}

static bool is_tilde_user_char(char ch) {
  return isalnum((unsigned char)ch) || ch == '_' || ch == '-' || ch == '.';
}

static void append_tilde_literal_char(char **buf, size_t *len, size_t *cap,
                                      char ch) {
  if (ch == ' ') {
    append_char(buf, len, cap, QUOTED_IFS_SPACE);
    return;
  }
  if (ch == '\t') {
    append_char(buf, len, cap, QUOTED_IFS_TAB);
    return;
  }
  if (ch == '\n') {
    append_char(buf, len, cap, QUOTED_IFS_NEWLINE);
    return;
  }
  if (ch == '*') {
    append_char(buf, len, cap, QUOTED_GLOB_STAR);
    return;
  }
  if (ch == '?') {
    append_char(buf, len, cap, QUOTED_GLOB_QMARK);
    return;
  }
  if (ch == '[') {
    append_char(buf, len, cap, QUOTED_GLOB_LBRACK);
    return;
  }
  append_char(buf, len, cap, QUOTED_LITERAL_PREFIX);
  append_char(buf, len, cap, ch);
}

static void append_tilde_literal(char **buf, size_t *len, size_t *cap,
                                 const char *value) {
  size_t i;

  for (i = 0; value[i] != '\0'; i++) {
    append_tilde_literal_char(buf, len, cap, value[i]);
  }
}

static bool try_tilde_expansion(const char *in, size_t *i, bool assignment_context,
                                char **buf, size_t *len, size_t *cap) {
  size_t name_start;
  size_t name_end;
  char delim;
  const char *home;
  char *name;
  struct passwd *pw;

  if (in[*i] != '~') {
    return false;
  }

  name_start = *i + 1;
  name_end = name_start;
  while (is_tilde_user_char(in[name_end])) {
    name_end++;
  }

  delim = in[name_end];
  if (delim != '\0' && delim != '/' && !(assignment_context && delim == ':')) {
    return false;
  }

  if (name_end == name_start) {
    home = getenv("HOME");
    if (home == NULL) {
      return false;
    }
    append_tilde_literal(buf, len, cap, home);
  } else {
    name = arena_xmalloc((name_end - name_start) + 1);
    memcpy(name, in + name_start, name_end - name_start);
    name[name_end - name_start] = '\0';
    pw = getpwnam(name);
    free(name);
    if (pw == NULL || pw->pw_dir == NULL) {
      return false;
    }
    append_tilde_literal(buf, len, cap, pw->pw_dir);
  }

  if (delim == '/' && *len > 0 && (*buf)[*len - 1] == '/') {
    name_end++;
  }
  *i = name_end;
  return true;
}

static bool find_command_substitution_close(const char *in, size_t start,
                                            size_t *close_out) {
  size_t i;
  char quote;
  bool dollar_single;
  bool in_comment;

  if (in[start] != '$' || in[start + 1] != '(') {
    return false;
  }

  quote = '\0';
  dollar_single = false;
  in_comment = false;
  for (i = start + 2; in[i] != '\0'; i++) {
    size_t inner_len;
    char *inner;
    char *inner_with_candidate;
    bool in_candidate_comment;
    int need_more;
    char ch;

    ch = in[i];
    if (in_comment) {
      if (ch == '\n') {
        in_comment = false;
      }
      continue;
    }
    if (dollar_single) {
      if (ch == '\\' && in[i + 1] != '\0') {
        i++;
        continue;
      }
      if (ch == '\'') {
        dollar_single = false;
      }
      continue;
    }
    if (quote == '\'') {
      if (ch == '\'') {
        quote = '\0';
      }
      continue;
    }
    if (quote == '"') {
      if (ch == '\\' && in[i + 1] != '\0') {
        i++;
        continue;
      }
      if (ch == '"') {
        quote = '\0';
      }
      continue;
    }
    if (ch == '\\' && in[i + 1] != '\0') {
      i++;
      continue;
    }
    if (ch == '$' && in[i + 1] == '\'') {
      dollar_single = true;
      i++;
      continue;
    }
    if (ch == '\'' || ch == '"') {
      quote = ch;
      continue;
    }
    if (ch == '#' && (i == start + 2 || isspace((unsigned char)in[i - 1]) ||
                      in[i - 1] == ';' || in[i - 1] == '&' ||
                      in[i - 1] == '|' || in[i - 1] == '(' ||
                      in[i - 1] == ')' || in[i - 1] == '{' ||
                      in[i - 1] == '}')) {
      in_comment = true;
      continue;
    }

    if (ch != ')') {
      continue;
    }

    inner_len = i - (start + 2);
    inner = arena_xmalloc(inner_len + 1);
    if (inner_len > 0) {
      memcpy(inner, in + start + 2, inner_len);
    }
    inner[inner_len] = '\0';

    need_more = shell_needs_more_input_text(inner, inner_len);
    inner_with_candidate = arena_xmalloc(inner_len + 2);
    if (inner_len > 0) {
      memcpy(inner_with_candidate, in + start + 2, inner_len);
    }
    inner_with_candidate[inner_len] = ')';
    inner_with_candidate[inner_len + 1] = '\0';
    in_candidate_comment =
        shell_position_in_comment(inner_with_candidate, inner_len + 1, inner_len);
    free(inner_with_candidate);
    free(inner);

    if (!in_candidate_comment && need_more == 0) {
      *close_out = i;
      return true;
    }
  }

  return false;
}

static size_t skip_command_subst_token(const char *token, size_t pos) {
  size_t i;
  int depth;
  char quote;

  if (token[pos] != '$' || token[pos + 1] != '(') {
    return pos + 1;
  }

  i = pos + 2;
  depth = 1;
  quote = '\0';
  while (token[i] != '\0') {
    char ch;

    ch = token[i];
    if (quote == '\0') {
      if (ch == '\\' && token[i + 1] != '\0') {
        i += 2;
        continue;
      }
      if (ch == '\'' || ch == '"') {
        quote = ch;
        i++;
        continue;
      }
      if (ch == '(') {
        depth++;
      } else if (ch == ')') {
        depth--;
        if (depth == 0) {
          return i + 1;
        }
      }
      i++;
      continue;
    }

    if (quote == '\'' && ch == '\'') {
      quote = '\0';
    } else if (quote == '"' && ch == '"') {
      quote = '\0';
    } else if (ch == '\\' && token[i + 1] != '\0') {
      i += 2;
      continue;
    }
    i++;
  }

  return i;
}

static size_t skip_braced_param_token(const char *token, size_t pos) {
  size_t i;
  int depth;
  char quote;

  if (token[pos] != '$' || token[pos + 1] != '{') {
    return pos + 1;
  }

  i = pos + 2;
  depth = 1;
  quote = '\0';
  while (token[i] != '\0') {
    char ch;

    ch = token[i];
    if (quote == '\0') {
      if (ch == '\\' && token[i + 1] != '\0') {
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
          return i + 1;
        }
      }
      i++;
      continue;
    }

    if (quote == '\'' && ch == '\'') {
      quote = '\0';
    } else if (quote == '"' && ch == '"') {
      quote = '\0';
    } else if (ch == '\\' && token[i + 1] != '\0') {
      i += 2;
      continue;
    }
    i++;
  }

  return i;
}

static size_t skip_backtick_token(const char *token, size_t pos) {
  size_t i;

  if (token[pos] != '`') {
    return pos + 1;
  }

  i = pos + 1;
  while (token[i] != '\0') {
    if (token[i] == '\\' && token[i + 1] != '\0') {
      i += 2;
      continue;
    }
    if (token[i] == '`') {
      return i + 1;
    }
    i++;
  }
  return i;
}

static bool token_is_unquoted(const char *token) {
  size_t i;

  i = 0;
  while (token[i] != '\0') {
    if (token[i] == '$' && token[i + 1] == '(') {
      i = skip_command_subst_token(token, i);
      continue;
    }
    if (token[i] == '$' && token[i + 1] == '{') {
      i = skip_braced_param_token(token, i);
      continue;
    }
    if (token[i] == '`') {
      i = skip_backtick_token(token, i);
      continue;
    }
    if (token[i] == '$' && token[i + 1] == '\'') {
      /* Dollar-single-quote is a quoting form and suppresses splitting. */
      return false;
    }
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

static bool token_has_glob_meta(const char *token) {
  size_t i;

  i = 0;
  while (token[i] != '\0') {
    if (token[i] == QUOTED_LITERAL_PREFIX && token[i + 1] != '\0') {
      i += 2;
      continue;
    }
    if (token[i] == '\\' && token[i + 1] != '\0') {
      i += 2;
      continue;
    }
    if (token[i] == '*' || token[i] == '?' || token[i] == '[') {
      return true;
    }
    i++;
  }
  return false;
}

static bool token_has_runtime_expansion(const char *token) {
  size_t i;

  i = 0;
  while (token[i] != '\0') {
    if (token[i] == '\\' && token[i + 1] != '\0') {
      i += 2;
      continue;
    }
    if (token[i] == '$' || token[i] == '`') {
      return true;
    }
    i++;
  }
  return false;
}

static bool token_is_pure_quoted_at(const char *token) {
  size_t i;

  i = 0;
  if (token[0] == '\0') {
    return false;
  }
  while (token[i] != '\0') {
    if (token[i] != '"') {
      return false;
    }
    i++;
    if (token[i] != '$' || token[i + 1] != '@') {
      return false;
    }
    i += 2;
    if (token[i] != '"') {
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

static char *normalize_backquote_command_text(const char *raw) {
  size_t i;
  char quote;
  bool pseudo_dquote;
  char *out;
  size_t len;
  size_t cap;

  i = 0;
  quote = '\0';
  pseudo_dquote = false;
  out = NULL;
  len = 0;
  cap = 0;

  while (raw[i] != '\0') {
    if (raw[i] == '\\' && raw[i + 1] != '\0') {
      char next;
      size_t j;
      bool has_matching_escaped_quote;

      next = raw[i + 1];
      has_matching_escaped_quote = false;
      if (next == '"' && quote == '\0' && !pseudo_dquote) {
        j = i + 2;
        while (raw[j] != '\0' && !isspace((unsigned char)raw[j])) {
          if (raw[j] == '\\' && raw[j + 1] == '"') {
            has_matching_escaped_quote = true;
            break;
          }
          if (raw[j] == '\\' && raw[j + 1] != '\0') {
            j += 2;
            continue;
          }
          j++;
        }
      }

      if (next == '$' || next == '`' || next == '\\' || next == '\n' ||
          (next == '"' && quote == '\0' &&
           (pseudo_dquote || has_matching_escaped_quote))) {
        if (next != '\n') {
          append_char(&out, &len, &cap, next);
        }
        if (next == '"' && quote == '\0') {
          pseudo_dquote = !pseudo_dquote;
        }
        i += 2;
        continue;
      }
    }

    append_char(&out, &len, &cap, raw[i]);
    if (quote == '\0' && (raw[i] == '\'' || raw[i] == '"')) {
      quote = raw[i];
    } else if (quote != '\0' && raw[i] == quote) {
      quote = '\0';
    }
    i++;
  }

  if (out == NULL) {
    out = arena_xstrdup("");
  }
  return out;
}

static int run_arithmetic_expansion(const char *expr, char **out_value,
                                    int *status_out,
                                    struct shell_state *state) {
  char *expanded_expr;
  long value;
  char text[64];

  if (expand_token(expr, state, &expanded_expr, false, false) != 0) {
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

static void mark_noninteractive_expansion_fatal(struct shell_state *state,
                                                int status) {
  if (!state->interactive) {
    state->should_exit = true;
    state->exit_status = status;
  }
}

static bool is_numeric_parameter_name(const char *name) {
  size_t i;

  if (name == NULL || name[0] == '\0') {
    return false;
  }
  for (i = 0; name[i] != '\0'; i++) {
    if (!isdigit((unsigned char)name[i])) {
      return false;
    }
  }
  return true;
}

static bool is_special_parameter_name(const char *name) {
  return strcmp(name, "$") == 0 || strcmp(name, "?") == 0 ||
         strcmp(name, "!") == 0 || strcmp(name, "#") == 0 ||
         strcmp(name, "@") == 0 || strcmp(name, "*") == 0 ||
         strcmp(name, "-") == 0 || strcmp(name, "0") == 0 ||
         is_numeric_parameter_name(name);
}

static int append_parameter(const char *name, size_t nlen,
                            struct shell_state *state, char **buf, size_t *len,
                            size_t *cap, bool quoted_context) {
    char *tmp;
    const char *val;

    tmp = arena_xmalloc(nlen + 1);
    memcpy(tmp, name, nlen);
    tmp[nlen] = '\0';

    if (strcmp(tmp, "?") == 0) {
        char num[32];
        snprintf(num, sizeof(num), "%d", state->last_status);
        if (quoted_context) {
          append_tilde_literal(buf, len, cap, num);
        } else {
          append_str(buf, len, cap, num);
        }
        free(tmp);
        return 0;
    }

    if (strcmp(tmp, "$") == 0) {
        char num[32];
        snprintf(num, sizeof(num), "%ld", (long)state->shell_pid);
        if (quoted_context) {
          append_tilde_literal(buf, len, cap, num);
        } else {
          append_str(buf, len, cap, num);
        }
        free(tmp);
        return 0;
    }

    if (strcmp(tmp, "!") == 0) {
        if (state->last_async_pid > 0) {
            char num[32];

            snprintf(num, sizeof(num), "%ld", (long)state->last_async_pid);
            if (quoted_context) {
              append_tilde_literal(buf, len, cap, num);
            } else {
              append_str(buf, len, cap, num);
            }
        }
        free(tmp);
        return 0;
    }

    if (strcmp(tmp, "#") == 0) {
        char num[32];
        snprintf(num, sizeof(num), "%zu", state->positional_count);
        if (quoted_context) {
          append_tilde_literal(buf, len, cap, num);
        } else {
          append_str(buf, len, cap, num);
        }
        free(tmp);
        return 0;
    }

    if (strcmp(tmp, "-") == 0) {
        char options_buf[64];

        options_format_dollar_minus(state, options_buf, sizeof(options_buf));
        if (quoted_context) {
          append_tilde_literal(buf, len, cap, options_buf);
        } else {
          append_str(buf, len, cap, options_buf);
        }
        free(tmp);
        return 0;
    }

    if (strcmp(tmp, "*") == 0) {
        size_t i;
        char sep;
        bool use_sep;

        val = getenv("IFS");
        if (!quoted_context && val != NULL && val[0] == '\0') {
            for (i = 0; i < state->positional_count; i++) {
                if (i > 0) {
                    append_char(buf, len, cap, PARAM_AT_SPLIT);
                }
                append_str(buf, len, cap, state->positional_params[i]);
            }
            free(tmp);
            return 0;
        }

        sep = ' ';
        use_sep = true;
        if (val != NULL) {
            if (val[0] == '\0') {
                use_sep = false;
            } else {
                sep = val[0];
            }
        }

        for (i = 0; i < state->positional_count; i++) {
            if (i > 0 && use_sep) {
                if (quoted_context) {
                  append_tilde_literal_char(buf, len, cap, sep);
                } else {
                  append_char(buf, len, cap, sep);
                }
            }
            if (quoted_context) {
              append_tilde_literal(buf, len, cap, state->positional_params[i]);
            } else {
              append_str(buf, len, cap, state->positional_params[i]);
            }
        }
        free(tmp);
        return 0;
    }

    if (strcmp(tmp, "@") == 0) {
        size_t i;

        for (i = 0; i < state->positional_count; i++) {
            if (i > 0) {
                append_char(buf, len, cap, PARAM_AT_SPLIT);
            }
            if (quoted_context) {
                append_tilde_literal(buf, len, cap, state->positional_params[i]);
            } else {
                append_str(buf, len, cap, state->positional_params[i]);
            }
        }
        free(tmp);
        return 0;
    }

    if (strcmp(tmp, "0") == 0) {
        val = getenv("0");
        if (val != NULL) {
            if (quoted_context) {
              append_tilde_literal(buf, len, cap, val);
            } else {
              append_str(buf, len, cap, val);
            }
        }
        free(tmp);
        return 0;
    }

    if (isdigit((unsigned char)tmp[0])) {
        char *end;
        unsigned long n;

        n = strtoul(tmp, &end, 10);
        if (end != tmp && *end == '\0' && n > 0 && n <= state->positional_count) {
            if (quoted_context) {
              append_tilde_literal(buf, len, cap, state->positional_params[n - 1]);
            } else {
              append_str(buf, len, cap, state->positional_params[n - 1]);
            }
        }
        free(tmp);
        return 0;
    }

    val = getenv(tmp);
    if (val != NULL) {
        if (quoted_context) {
          append_tilde_literal(buf, len, cap, val);
        } else {
          append_str(buf, len, cap, val);
        }
    } else if (state->nounset) {
        posish_errorf("%s: parameter not set", tmp);
        mark_noninteractive_expansion_fatal(state, 1);
        free(tmp);
        return -1;
    }

    free(tmp);
    return 0;
}

static void append_context_string(char **buf, size_t *len, size_t *cap,
                                  const char *value, bool quoted_context) {
  if (quoted_context) {
    append_tilde_literal(buf, len, cap, value);
  } else {
    append_str(buf, len, cap, value);
  }
}

static int append_expanded_fragment(const char *expr, size_t start, size_t elen,
                                    struct shell_state *state, char **buf,
                                    size_t *len, size_t *cap,
                                    bool in_double_quotes);
static void restore_quoted_ifs_markers(char *s);
static int expand_pattern_fragment(const char *expr, size_t start, size_t elen,
                                   struct shell_state *state, char **out);

static bool should_tilde_expand(const char *s, bool in_double_quotes) {
  return !in_double_quotes && s[0] == '~' && (s[1] == '\0' || s[1] == '/');
}

static char *maybe_tilde_expand_fragment(char *expanded, bool in_double_quotes) {
  const char *home;
  size_t hlen;
  size_t slen;
  char *out;

  if (!should_tilde_expand(expanded, in_double_quotes)) {
    return expanded;
  }

  home = getenv("HOME");
  if (home == NULL) {
    return expanded;
  }

  hlen = strlen(home);
  slen = strlen(expanded);
  out = arena_xmalloc(hlen + slen);
  memcpy(out, home, hlen);
  memcpy(out + hlen, expanded + 1, slen);
  free(expanded);
  return out;
}

static int append_expanded_fragment(const char *expr, size_t start, size_t elen,
                                    struct shell_state *state, char **buf,
                                    size_t *len, size_t *cap,
                                    bool in_double_quotes) {
  char *expanded_word;
  size_t wlen;
  char *word;
  int rc;

  wlen = elen - start;
  word = arena_xmalloc(wlen + 1);
  memcpy(word, expr + start, wlen);
  word[wlen] = '\0';

  rc = expand_token(word, state, &expanded_word, in_double_quotes, false);
  if (rc != 0) {
    free(word);
    return -1;
  }

  expanded_word = maybe_tilde_expand_fragment(expanded_word, in_double_quotes);
  /*
   * `expand_token` already applies quote-context protection. Re-encoding here
   * would leak marker bytes for nested braced expansions.
   */
  append_str(buf, len, cap, expanded_word);
  free(expanded_word);
  free(word);
  return 0;
}

static int expand_fragment_to_string(const char *expr, size_t start, size_t elen,
                                     struct shell_state *state, char **out,
                                     bool in_double_quotes) {
  size_t wlen;
  char *word;
  char *expanded;
  int rc;

  wlen = elen - start;
  word = arena_xmalloc(wlen + 1);
  memcpy(word, expr + start, wlen);
  word[wlen] = '\0';

  rc = expand_token(word, state, &expanded, in_double_quotes, false);
  free(word);
  if (rc != 0) {
    return rc;
  }

  expanded = maybe_tilde_expand_fragment(expanded, in_double_quotes);
  *out = expanded;
  return rc;
}

static char pattern_marker_for_char(char ch) {
  switch (ch) {
  case '*':
    return PATTERN_LIT_STAR;
  case '?':
    return PATTERN_LIT_QMARK;
  case '[':
    return PATTERN_LIT_LBRACK;
  case ']':
    return PATTERN_LIT_RBRACK;
  case '\\':
    return PATTERN_LIT_BSLASH;
  default:
    return '\0';
  }
}

static char *mark_pattern_escapes(const char *word) {
  char *marked;
  size_t i;
  size_t len;
  size_t cap;
  char quote;

  marked = NULL;
  len = 0;
  cap = 0;
  quote = '\0';
  for (i = 0; word[i] != '\0'; i++) {
    char ch;

    ch = word[i];
    if (quote == '\0') {
      if (ch == '$' && word[i + 1] == '(') {
        size_t end;

        end = skip_command_subst_token(word, i);
        while (i < end) {
          append_char(&marked, &len, &cap, word[i]);
          i++;
        }
        i--;
        continue;
      }
      if (ch == '$' && word[i + 1] == '{') {
        size_t end;

        end = skip_braced_param_token(word, i);
        while (i < end) {
          append_char(&marked, &len, &cap, word[i]);
          i++;
        }
        i--;
        continue;
      }
      if (ch == '`') {
        size_t end;

        end = skip_backtick_token(word, i);
        while (i < end) {
          append_char(&marked, &len, &cap, word[i]);
          i++;
        }
        i--;
        continue;
      }
      if (ch == '\\' && word[i + 1] != '\0') {
        char marker;

        marker = pattern_marker_for_char(word[i + 1]);
        if (marker != '\0') {
          append_char(&marked, &len, &cap, '\\');
          append_char(&marked, &len, &cap, marker);
          i++;
          continue;
        }
      } else if (ch == '\'') {
        quote = '\'';
      } else if (ch == '"') {
        quote = '"';
      }
    } else if (quote == '\'' && ch == '\'') {
      quote = '\0';
    } else if (quote == '"' && ch == '"') {
      quote = '\0';
    }
    append_char(&marked, &len, &cap, ch);
  }
  if (marked == NULL) {
    marked = arena_xstrdup("");
  }
  return marked;
}

static char *unmark_pattern_escapes(const char *expanded) {
  char *out;
  size_t i;
  size_t len;
  size_t cap;

  out = NULL;
  len = 0;
  cap = 0;
  for (i = 0; expanded[i] != '\0'; i++) {
    char ch;

    ch = expanded[i];
    if (ch == QUOTED_EMPTY_MARK) {
      continue;
    }
    if (ch == QUOTED_LITERAL_PREFIX && expanded[i + 1] != '\0') {
      char literal;

      literal = expanded[++i];
      if (literal == PATTERN_LIT_STAR) {
        append_char(&out, &len, &cap, '\\');
        append_char(&out, &len, &cap, '*');
        continue;
      }
      if (literal == PATTERN_LIT_QMARK) {
        append_char(&out, &len, &cap, '\\');
        append_char(&out, &len, &cap, '?');
        continue;
      }
      if (literal == PATTERN_LIT_LBRACK) {
        append_char(&out, &len, &cap, '\\');
        append_char(&out, &len, &cap, '[');
        continue;
      }
      if (literal == PATTERN_LIT_RBRACK) {
        append_char(&out, &len, &cap, '\\');
        append_char(&out, &len, &cap, ']');
        continue;
      }
      if (literal == PATTERN_LIT_BSLASH) {
        append_char(&out, &len, &cap, '\\');
        append_char(&out, &len, &cap, '\\');
        continue;
      }
      if (literal == '*' || literal == '?' || literal == '[' || literal == ']' ||
          literal == '\\') {
        append_char(&out, &len, &cap, '\\');
      }
      append_char(&out, &len, &cap, literal);
      continue;
    }
    if (ch == QUOTED_IFS_SPACE) {
      append_char(&out, &len, &cap, ' ');
      continue;
    }
    if (ch == QUOTED_IFS_TAB) {
      append_char(&out, &len, &cap, '\t');
      continue;
    }
    if (ch == QUOTED_IFS_NEWLINE) {
      append_char(&out, &len, &cap, '\n');
      continue;
    }
    if (ch == QUOTED_GLOB_STAR) {
      append_char(&out, &len, &cap, '\\');
      append_char(&out, &len, &cap, '*');
      continue;
    }
    if (ch == QUOTED_GLOB_QMARK) {
      append_char(&out, &len, &cap, '\\');
      append_char(&out, &len, &cap, '?');
      continue;
    }
    if (ch == QUOTED_GLOB_LBRACK) {
      append_char(&out, &len, &cap, '\\');
      append_char(&out, &len, &cap, '[');
      continue;
    }
    if (ch == PATTERN_LIT_STAR) {
      append_char(&out, &len, &cap, '\\');
      append_char(&out, &len, &cap, '*');
      continue;
    }
    if (ch == PATTERN_LIT_QMARK) {
      append_char(&out, &len, &cap, '\\');
      append_char(&out, &len, &cap, '?');
      continue;
    }
    if (ch == PATTERN_LIT_LBRACK) {
      append_char(&out, &len, &cap, '\\');
      append_char(&out, &len, &cap, '[');
      continue;
    }
    if (ch == PATTERN_LIT_RBRACK) {
      append_char(&out, &len, &cap, '\\');
      append_char(&out, &len, &cap, ']');
      continue;
    }
    if (ch == PATTERN_LIT_BSLASH) {
      append_char(&out, &len, &cap, '\\');
      append_char(&out, &len, &cap, '\\');
      continue;
    }
    append_char(&out, &len, &cap, ch);
  }
  if (out == NULL) {
    out = arena_xstrdup("");
  }
  return out;
}

static int expand_pattern_fragment(const char *expr, size_t start, size_t elen,
                                   struct shell_state *state, char **out) {
  size_t wlen;
  char *word;
  char *marked;
  char *expanded;
  char *unmarked;
  int rc;

  wlen = elen - start;
  word = arena_xmalloc(wlen + 1);
  memcpy(word, expr + start, wlen);
  word[wlen] = '\0';

  marked = mark_pattern_escapes(word);
  free(word);

  rc = expand_token(marked, state, &expanded, false, false);
  free(marked);
  if (rc != 0) {
    return rc;
  }

  unmarked = unmark_pattern_escapes(expanded);
  free(expanded);
  *out = unmarked;
  return 0;
}

static int append_braced_parameter(const char *expr, size_t elen,
                                   struct shell_state *state, char **buf,
                                   size_t *len, size_t *cap,
                                   bool in_double_quotes) {
    enum braced_op {
        BRACED_NONE,
        BRACED_LENGTH,
        BRACED_MINUS,
        BRACED_COLON_MINUS,
        BRACED_PLUS,
        BRACED_COLON_PLUS,
        BRACED_ASSIGN,
        BRACED_COLON_ASSIGN,
        BRACED_ERROR,
        BRACED_COLON_ERROR,
        BRACED_HASH,
        BRACED_DBL_HASH,
        BRACED_PERCENT,
        BRACED_DBL_PERCENT
    };
    enum braced_op op;
    size_t name_start;
    size_t name_len;
    size_t op_pos;
    size_t word_start;
    size_t i;
    char *name;
    const char *value;
    bool is_set;
    bool is_nonempty;
    bool name_is_special;
    unsigned long positional_index;
    char *endptr;

    op = BRACED_NONE;
    name_start = 0;
    name_len = elen;
    op_pos = elen;

    {
        bool parse_as_length;

        parse_as_length = false;
        if (elen > 1 && expr[0] == '#') {
            size_t p;

            p = 1;
            if (is_short_parameter_char(expr[p])) {
                if (isdigit((unsigned char)expr[p])) {
                    while (p < elen && isdigit((unsigned char)expr[p])) {
                        p++;
                    }
                } else {
                    p++;
                }
            } else if (isalpha((unsigned char)expr[p]) || expr[p] == '_') {
                while (p < elen &&
                       (isalnum((unsigned char)expr[p]) || expr[p] == '_')) {
                    p++;
                }
            }
            parse_as_length = p == elen;
        }

        if (parse_as_length) {
            op = BRACED_LENGTH;
            name_start = 1;
            name_len = elen - 1;
        } else {
        for (i = 0; i < elen; i++) {
            if (expr[i] == ':' && i + 1 < elen) {
                if (expr[i + 1] == '-') {
                    op = BRACED_COLON_MINUS;
                    op_pos = i;
                    break;
                }
                if (expr[i + 1] == '+') {
                    op = BRACED_COLON_PLUS;
                    op_pos = i;
                    break;
                }
                if (expr[i + 1] == '=') {
                    op = BRACED_COLON_ASSIGN;
                    op_pos = i;
                    break;
                }
                if (expr[i + 1] == '?') {
                    op = BRACED_COLON_ERROR;
                    op_pos = i;
                    break;
                }
            }
            if (expr[i] == '-') {
                op = BRACED_MINUS;
                op_pos = i;
                break;
            }
            if (expr[i] == '+') {
                op = BRACED_PLUS;
                op_pos = i;
                break;
            }
            if (expr[i] == '=') {
                op = BRACED_ASSIGN;
                op_pos = i;
                break;
            }
            if (expr[i] == '?') {
                op = BRACED_ERROR;
                op_pos = i;
                break;
            }
            if (i > 0 && expr[i] == '#') {
                op = (i + 1 < elen && expr[i + 1] == '#') ? BRACED_DBL_HASH
                                                           : BRACED_HASH;
                op_pos = i;
                break;
            }
            if (i > 0 && expr[i] == '%') {
                op = (i + 1 < elen && expr[i + 1] == '%') ? BRACED_DBL_PERCENT
                                                           : BRACED_PERCENT;
                op_pos = i;
                break;
            }
        }
        if (op != BRACED_NONE) {
            name_len = op_pos;
        }
        }
    }

    if (name_len == 0) {
        return append_parameter(expr, elen, state, buf, len, cap,
                                in_double_quotes);
    }

    name = arena_xmalloc(name_len + 1);
    memcpy(name, expr + name_start, name_len);
    name[name_len] = '\0';

    name_is_special = is_special_parameter_name(name);
    positional_index = 0;

    if (!vars_is_name_valid(name) && !name_is_special) {
        free(name);
        return append_parameter(expr, elen, state, buf, len, cap,
                                in_double_quotes);
    }

    if (strcmp(name, "0") == 0) {
        value = getenv("0");
        is_set = value != NULL;
        is_nonempty = value != NULL && value[0] != '\0';
    } else if (is_numeric_parameter_name(name)) {
        errno = 0;
        positional_index = strtoul(name, &endptr, 10);
        if (errno == 0 && endptr != name && *endptr == '\0' &&
            positional_index > 0 && positional_index <= state->positional_count) {
            value = state->positional_params[positional_index - 1];
            is_set = true;
        } else {
            value = NULL;
            is_set = false;
        }
        is_nonempty = value != NULL && value[0] != '\0';
    } else if (name_is_special) {
        if (strcmp(name, "*") == 0 || strcmp(name, "@") == 0) {
            is_set = state->positional_count > 0;
            is_nonempty = is_set;
        } else if (strcmp(name, "!") == 0) {
            is_set = true;
            is_nonempty = state->last_async_pid > 0;
        } else if (strcmp(name, "-") == 0) {
            char options_buf[64];

            options_format_dollar_minus(state, options_buf, sizeof(options_buf));
            is_set = true;
            is_nonempty = options_buf[0] != '\0';
        } else {
            is_set = true;
            is_nonempty = true;
        }
        value = NULL;
    } else {
        value = getenv(name);
        is_set = value != NULL;
        is_nonempty = value != NULL && value[0] != '\0';
    }

    if (op == BRACED_NONE) {
        if (!is_set && state->nounset) {
            posish_errorf("%s: parameter not set", name);
            mark_noninteractive_expansion_fatal(state, 1);
            free(name);
            return -1;
        }
        if (is_set) {
            if (name_is_special) {
                int rc;

                rc = append_parameter(name, name_len, state, buf, len, cap,
                                      in_double_quotes);
                free(name);
                return rc;
            }
            append_context_string(buf, len, cap, value, in_double_quotes);
        }
        free(name);
        return 0;
    }

    if (op == BRACED_LENGTH) {
        char text[32];

        if (name_is_special) {
            char *special_text;
            size_t special_len;
            size_t special_cap;
            int rc;

            special_text = NULL;
            special_len = 0;
            special_cap = 0;
            rc = append_parameter(name, name_len, state, &special_text, &special_len,
                                  &special_cap, false);
            if (rc != 0) {
                free(name);
                free(special_text);
                return -1;
            }
            if (special_text == NULL) {
                special_text = arena_xstrdup("");
            }
            restore_quoted_ifs_markers(special_text);
            snprintf(text, sizeof(text), "%zu", strlen(special_text));
            free(special_text);
            append_context_string(buf, len, cap, text, in_double_quotes);
            free(name);
            return 0;
        }

        if (!is_set && state->nounset) {
            posish_errorf("%s: parameter not set", name);
            mark_noninteractive_expansion_fatal(state, 1);
            free(name);
            return -1;
        }
        snprintf(text, sizeof(text), "%zu", is_set ? strlen(value) : 0);
        append_context_string(buf, len, cap, text, in_double_quotes);
        free(name);
        return 0;
    }

    if (op == BRACED_MINUS || op == BRACED_COLON_MINUS) {
        bool use_default;
        int rc;

        word_start = op == BRACED_COLON_MINUS ? op_pos + 2 : op_pos + 1;
        use_default = op == BRACED_COLON_MINUS ? !is_nonempty : !is_set;
        if (use_default) {
            rc = append_expanded_fragment(expr, word_start, elen, state, buf, len,
                                          cap, in_double_quotes);
            free(name);
            return rc;
        }
        if (is_set) {
            if (name_is_special) {
                int rc;

                rc = append_parameter(name, name_len, state, buf, len, cap,
                                      in_double_quotes);
                free(name);
                return rc;
            }
            append_context_string(buf, len, cap, value, in_double_quotes);
        }
        free(name);
        return 0;
    }

    if (op == BRACED_PLUS || op == BRACED_COLON_PLUS) {
        bool use_alternate;
        int rc;

        word_start = op == BRACED_COLON_PLUS ? op_pos + 2 : op_pos + 1;
        use_alternate = op == BRACED_COLON_PLUS ? is_nonempty : is_set;
        if (use_alternate) {
            rc = append_expanded_fragment(expr, word_start, elen, state, buf, len,
                                          cap, in_double_quotes);
            free(name);
            return rc;
        }
        free(name);
        return 0;
    }

    if (op == BRACED_ASSIGN || op == BRACED_COLON_ASSIGN) {
        bool should_assign;

        word_start = op == BRACED_COLON_ASSIGN ? op_pos + 2 : op_pos + 1;
        should_assign = op == BRACED_COLON_ASSIGN ? !is_nonempty : !is_set;
        if (should_assign && name_is_special) {
            posish_errorf("%s: cannot assign to special parameter", name);
            mark_noninteractive_expansion_fatal(state, 1);
            free(name);
            return -1;
        }
        if (should_assign) {
            char *expanded_word;
            int rc;

            rc = expand_fragment_to_string(expr, word_start, elen, state,
                                           &expanded_word, in_double_quotes);
            if (rc != 0) {
                free(name);
                return -1;
            }
            restore_quoted_ifs_markers(expanded_word);
            rc = vars_set_assignment(state, name, expanded_word, true);
            free(expanded_word);
            if (rc != 0) {
                free(name);
                return -1;
            }
            value = getenv(name);
            is_set = value != NULL;
        }
        if (is_set) {
            if (name_is_special) {
                int rc;

                rc = append_parameter(name, name_len, state, buf, len, cap,
                                      in_double_quotes);
                free(name);
                return rc;
            }
            append_context_string(buf, len, cap, value, in_double_quotes);
        }
        free(name);
        return 0;
    }

    if (op == BRACED_ERROR || op == BRACED_COLON_ERROR) {
        bool should_error;
        const char *msg_default;
        char *msg_expanded;
        int rc;

        word_start = op == BRACED_COLON_ERROR ? op_pos + 2 : op_pos + 1;
        should_error = op == BRACED_COLON_ERROR ? !is_nonempty : !is_set;
        if (!should_error) {
            if (is_set) {
                if (name_is_special) {
                    int rc2;

                    rc2 = append_parameter(name, name_len, state, buf, len, cap,
                                           in_double_quotes);
                    free(name);
                    return rc2;
                }
                append_context_string(buf, len, cap, value, in_double_quotes);
            }
            free(name);
            return 0;
        }

        msg_default = "parameter not set";
        msg_expanded = NULL;
        if (word_start < elen) {
            rc = expand_fragment_to_string(expr, word_start, elen, state,
                                           &msg_expanded, in_double_quotes);
            if (rc != 0) {
                free(name);
                return -1;
            }
            restore_quoted_ifs_markers(msg_expanded);
        }
        if (msg_expanded != NULL && msg_expanded[0] != '\0') {
            posish_errorf("%s: %s", name, msg_expanded);
        } else {
            posish_errorf("%s: %s", name, msg_default);
        }
        mark_noninteractive_expansion_fatal(state, 1);
        free(msg_expanded);
        free(name);
        return -1;
    }

    if (op == BRACED_HASH || op == BRACED_DBL_HASH || op == BRACED_PERCENT ||
        op == BRACED_DBL_PERCENT) {
        char *expanded_pattern;
        char *special_value;
        size_t pattern_pos;
        size_t vlen;
        size_t best;

        special_value = NULL;
        if (!is_set) {
            if (state->nounset) {
                posish_errorf("%s: parameter not set", name);
                mark_noninteractive_expansion_fatal(state, 1);
                free(name);
                return -1;
            }
            value = "";
        } else if (name_is_special) {
            size_t sv_len;
            size_t sv_cap;
            int rc;

            sv_len = 0;
            sv_cap = 0;
            rc = append_parameter(name, name_len, state, &special_value, &sv_len,
                                  &sv_cap, false);
            if (rc != 0) {
                free(name);
                free(special_value);
                return -1;
            }
            if (special_value == NULL) {
                special_value = arena_xstrdup("");
            }
            restore_quoted_ifs_markers(special_value);
            value = special_value;
        }

        pattern_pos =
            op == BRACED_DBL_HASH || op == BRACED_DBL_PERCENT ? op_pos + 2
                                                               : op_pos + 1;
        if (expand_pattern_fragment(expr, pattern_pos, elen, state,
                                    &expanded_pattern) != 0) {
            free(name);
            return -1;
        }

        vlen = strlen(value);
        best = (size_t)-1;
        if (op == BRACED_HASH || op == BRACED_DBL_HASH) {
            for (i = 0; i <= vlen; i++) {
                char *candidate;
                bool matched;

                candidate = arena_xmalloc(i + 1);
                memcpy(candidate, value, i);
                candidate[i] = '\0';
                matched = fnmatch(expanded_pattern, candidate, 0) == 0;
                free(candidate);
                if (!matched) {
                    continue;
                }
                if (op == BRACED_HASH) {
                    best = i;
                    break;
                }
                best = i;
            }

            if (best == (size_t)-1) {
                append_context_string(buf, len, cap, value, in_double_quotes);
            } else {
                append_context_string(buf, len, cap, value + best,
                                      in_double_quotes);
            }
        } else {
            for (i = 0; i <= vlen; i++) {
                const char *candidate;
                bool matched;

                candidate = value + i;
                matched = fnmatch(expanded_pattern, candidate, 0) == 0;
                if (!matched) {
                    continue;
                }
                if (op == BRACED_PERCENT) {
                    best = i;
                } else {
                    best = i;
                    break;
                }
            }

            if (best == (size_t)-1) {
                append_context_string(buf, len, cap, value, in_double_quotes);
            } else {
                char *trimmed;

                trimmed = arena_xmalloc(best + 1);
                memcpy(trimmed, value, best);
                trimmed[best] = '\0';
                append_context_string(buf, len, cap, trimmed, in_double_quotes);
                free(trimmed);
            }
        }

        free(expanded_pattern);
        free(special_value);
        free(name);
        return 0;
    }

    free(name);
    return append_parameter(expr, elen, state, buf, len, cap, in_double_quotes);
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

static void restore_quoted_ifs_markers(char *s) {
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

static int split_and_append_fields(const char *expanded, struct token_vec *out) {
  const char *ifs_env;
  const char *ifs;
  size_t pos;
  int appended;
  bool has_delimiter;

  ifs_env = getenv("IFS");
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

    /* Leading IFS-whitespace does not create empty fields. */
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
    restore_quoted_ifs_markers(field);
    out->items = xrealloc(out->items, sizeof(*out->items) * (out->len + 1));
    out->items[out->len++] = field;
    appended++;

    if (expanded[pos] == '\0') {
      break;
    }

    /*
     * Consume one full delimiter sequence. POSIX groups IFS-whitespace around
     * an IFS non-whitespace delimiter into a single separator.
     */
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

static bool expanded_has_split_delimiter(const char *expanded) {
  const char *ifs_env;
  const char *ifs;
  size_t pos;

  ifs_env = getenv("IFS");
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

static bool expanded_has_at_split_marker(const char *expanded) {
  size_t i;

  for (i = 0; expanded[i] != '\0'; i++) {
    if (expanded[i] == PARAM_AT_SPLIT) {
      return true;
    }
  }
  return false;
}

static int append_expanded_piece(char *piece, struct token_vec *out,
                                 bool split_fields) {
  if (split_fields) {
    int count;
    bool had_delim;

    had_delim = expanded_has_split_delimiter(piece);
    count = split_and_append_fields(piece, out);
    if (count > 0) {
      free(piece);
      return count;
    }
    if (had_delim) {
      free(piece);
      return 0;
    }
  }

  restore_quoted_ifs_markers(piece);
  out->items = xrealloc(out->items, sizeof(*out->items) * (out->len + 1));
  out->items[out->len++] = piece;
  return 1;
}

static int append_at_split_expansion(const char *expanded, struct token_vec *out,
                                     bool split_fields) {
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
      added_total += append_expanded_piece(piece, out, split_fields);
    }

    if (expanded[i] == '\0') {
      break;
    }
    start = i + 1;
  }

  return added_total;
}

static int expand_token(const char *in, struct shell_state *state, char **out,
                        bool dquote_mode, bool assignment_context) {
  size_t i;
  size_t assign_value_pos;
  bool assign_mode;
  bool tilde_allowed;
  char *buf;
  size_t len;
  size_t cap;
  char quote;

  i = 0;
  assign_value_pos = 0;
  assign_mode = !dquote_mode && assignment_context &&
                assignment_value_start(in, &assign_value_pos);
  tilde_allowed = !dquote_mode && !assign_mode;
  buf = NULL;
  len = 0;
  cap = 0;
  quote = '\0';

  if (assign_mode && assign_value_pos > 0) {
    while (i < assign_value_pos) {
      append_char(&buf, &len, &cap, in[i]);
      i++;
    }
    tilde_allowed = true;
  }

  while (in[i] != '\0') {
    if (quote == '\0' && tilde_allowed && in[i] == '~') {
      if (try_tilde_expansion(in, &i, assign_mode, &buf, &len, &cap)) {
        tilde_allowed = false;
        continue;
      }
    }

    if (quote == '\'') {
      if (in[i] == '\'') {
        append_char(&buf, &len, &cap, QUOTED_EMPTY_MARK);
        quote = '\0';
        i++;
        continue;
      }
      append_tilde_literal_char(&buf, &len, &cap, in[i]);
      i++;
      continue;
    }

    if (quote == '"') {
      if (in[i] == '"') {
        append_char(&buf, &len, &cap, QUOTED_EMPTY_MARK);
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
        append_tilde_literal_char(&buf, &len, &cap, in[i]);
        i++;
        continue;
      }
    } else {
      if (!dquote_mode && in[i] == '\'') {
        append_char(&buf, &len, &cap, QUOTED_EMPTY_MARK);
        quote = '\'';
        if (assign_mode) {
          tilde_allowed = false;
        }
        i++;
        continue;
      }
      if (in[i] == '"') {
        append_char(&buf, &len, &cap, QUOTED_EMPTY_MARK);
        quote = '"';
        if (assign_mode) {
          tilde_allowed = false;
        }
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
        if (dquote_mode) {
          if (in[i] == '$' || in[i] == '`' || in[i] == '"' || in[i] == '\\' ||
              in[i] == '}') {
            append_tilde_literal_char(&buf, &len, &cap, in[i]);
            i++;
            continue;
          }
          append_tilde_literal_char(&buf, &len, &cap, '\\');
          append_tilde_literal_char(&buf, &len, &cap, in[i]);
          i++;
          continue;
        }
        append_tilde_literal_char(&buf, &len, &cap, in[i]);
        if (assign_mode) {
          tilde_allowed = false;
        }
        i++;
        continue;
      }
    }

    if (in[i] == '$') {
      size_t next;

      next = skip_token_line_continuations(in, i + 1);
      if (!dquote_mode && in[next] == '\'') {
        if (next != i + 1) {
          i = next;
        }
        if (append_dollar_single_quoted(in, &i, &buf, &len, &cap) != 0) {
          free(buf);
          return -1;
        }
        continue;
      }

      if (in[next] == '{') {
        size_t j;
        int depth;
        char inner_quote;
        bool braced_dquote;

        j = next + 1;
        depth = 1;
        inner_quote = '\0';
        braced_dquote = dquote_mode || quote == '"';
        while (in[j] != '\0' && depth > 0) {
          char ch;

          ch = in[j];
          if (inner_quote == '\0') {
            if (ch == '\\' && in[j + 1] != '\0') {
              j += 2;
              continue;
            }
            if (ch == '"' || (!braced_dquote && ch == '\'')) {
              inner_quote = ch;
              j++;
              continue;
            }
            if (ch == '{') {
              depth++;
            } else if (ch == '}') {
              depth--;
              if (depth == 0) {
                break;
              }
            }
            j++;
            continue;
          }

          if (inner_quote == '\'' && ch == '\'') {
            inner_quote = '\0';
            j++;
            continue;
          }
          if (inner_quote == '"') {
            if (ch == '\\' && in[j + 1] != '\0') {
              j += 2;
              continue;
            }
            if (ch == '"') {
              inner_quote = '\0';
            }
          }
          j++;
        }
        if (depth != 0 || in[j] != '}') {
          posish_errorf("unterminated parameter expansion");
          free(buf);
          return -1;
        }

        if (j > next + 1) {
          size_t expr_len;
          char *expr;

          expr = collapse_line_continuations_copy(in + next + 1, j - (next + 1),
                                                  &expr_len);
          if (append_braced_parameter(expr, expr_len, state, &buf, &len, &cap,
                                      dquote_mode || quote == '"') != 0) {
            free(expr);
            free(buf);
            return -1;
          }
          free(expr);
        }
        i = j + 1;
        continue;
      }

      if (in[next] == '(') {
        if (in[next + 1] == '(') {
          size_t j;
          int depth;
          char *expr;
          char *value;
          int cmd_status;

          j = next + 2;
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

          expr = arena_xmalloc((j - (next + 2)) + 1);
          memcpy(expr, in + next + 2, j - (next + 2));
          expr[j - (next + 2)] = '\0';

          if (run_arithmetic_expansion(expr, &value, &cmd_status, state) != 0) {
            free(expr);
            free(buf);
            return -1;
          }
          append_context_string(&buf, &len, &cap, value,
                                dquote_mode || quote == '"');
          state->last_cmdsub_status = cmd_status;
          state->cmdsub_performed = true;
          free(value);
          free(expr);
          i = j + 2;
          continue;
        }

        size_t j;
        char *cmd;
        char *value;
        int cmd_status;

        if (!find_command_substitution_close(in, next - 1, &j)) {
          posish_errorf("unterminated command substitution");
          free(buf);
          return -1;
        }

        cmd = arena_xmalloc((j - (next + 1)) + 1);
        memcpy(cmd, in + next + 1, j - (next + 1));
        cmd[j - (next + 1)] = '\0';

          if (run_command_substitution(state, cmd, &value, &cmd_status) != 0) {
            free(cmd);
            free(buf);
            return -1;
          }

          append_context_string(&buf, &len, &cap, value,
                                dquote_mode || quote == '"');
          state->last_cmdsub_status = cmd_status;
          state->cmdsub_performed = true;
          free(value);
        free(cmd);
        i = j + 1;
        continue;
      }

      if (in[next] == '`') {
        append_char(&buf, &len, &cap, '$');
        i = next;
        continue;
      }

      if (is_short_parameter_char(in[next])) {
        if (append_parameter(in + next, 1, state, &buf, &len, &cap,
                             dquote_mode || quote == '"') != 0) {
          free(buf);
          return -1;
        }
        i = next + 1;
        continue;
      }

      if (isalpha((unsigned char)in[next]) || in[next] == '_') {
        size_t j;

        j = next;
        while (isalnum((unsigned char)in[j]) || in[j] == '_') {
          j++;
        }
        if (append_parameter(in + next, j - next, state, &buf, &len, &cap,
                             dquote_mode || quote == '"') != 0) {
          free(buf);
          return -1;
        }
        i = j;
        continue;
      }
    }

    if (in[i] == '`') {
      size_t j;
      char *cmd;
      char *normalized_cmd;
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
      normalized_cmd = normalize_backquote_command_text(cmd);
      free(cmd);
      cmd = normalized_cmd;

      if (run_command_substitution(state, cmd, &value, &cmd_status) != 0) {
        free(cmd);
        free(buf);
        return -1;
      }

      append_context_string(&buf, &len, &cap, value,
                            dquote_mode || quote == '"');
      state->last_cmdsub_status = cmd_status;
      state->cmdsub_performed = true;
      free(value);
      free(cmd);
      i = j + 1;
      continue;
    }

    if (dquote_mode || quote == '"') {
      append_tilde_literal_char(&buf, &len, &cap, in[i]);
    } else {
      append_char(&buf, &len, &cap, in[i]);
    }
    if (assign_mode) {
      if (quote == '\0' && in[i] == ':') {
        tilde_allowed = true;
      } else {
        tilde_allowed = false;
      }
    } else {
      tilde_allowed = false;
    }
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

int expand_heredoc_text(const char *in, struct shell_state *state, char **out) {
  size_t i;
  char *buf;
  size_t len;
  size_t cap;

  i = 0;
  buf = NULL;
  len = 0;
  cap = 0;

  while (in[i] != '\0') {
    if (in[i] == '\\') {
      if (in[i + 1] == '\n') {
        i += 2;
        continue;
      }
      if (in[i + 1] == '$' || in[i + 1] == '`' || in[i + 1] == '\\') {
        append_char(&buf, &len, &cap, in[i + 1]);
        i += 2;
        continue;
      }
      append_char(&buf, &len, &cap, '\\');
      i++;
      continue;
    }

    if (in[i] == '$') {
      if (in[i + 1] == '{') {
        size_t j;
        int depth;
        char inner_quote;

        j = i + 2;
        depth = 1;
        inner_quote = '\0';
        while (in[j] != '\0' && depth > 0) {
          char ch;

          ch = in[j];
          if (inner_quote == '\0') {
            if (ch == '\\' && in[j + 1] != '\0') {
              j += 2;
              continue;
            }
            if (ch == '"' || ch == '\'') {
              inner_quote = ch;
              j++;
              continue;
            }
            if (ch == '{') {
              depth++;
            } else if (ch == '}') {
              depth--;
              if (depth == 0) {
                break;
              }
            }
            j++;
            continue;
          }

          if (inner_quote == '\'' && ch == '\'') {
            inner_quote = '\0';
            j++;
            continue;
          }
          if (inner_quote == '"') {
            if (ch == '\\' && in[j + 1] != '\0') {
              j += 2;
              continue;
            }
            if (ch == '"') {
              inner_quote = '\0';
            }
          }
          j++;
        }
        if (depth != 0 || in[j] != '}') {
          posish_errorf("unterminated parameter expansion");
          free(buf);
          return -1;
        }

        if (j > i + 2) {
          size_t expr_len;
          char *expr;

          expr = collapse_line_continuations_copy(in + i + 2, j - (i + 2),
                                                  &expr_len);
          if (append_braced_parameter(expr, expr_len, state, &buf, &len, &cap,
                                      true) != 0) {
            free(expr);
            free(buf);
            return -1;
          }
          free(expr);
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
        char *cmd;
        char *value;
        int cmd_status;

        if (!find_command_substitution_close(in, i, &j)) {
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
        if (append_parameter(in + i + 1, 1, state, &buf, &len, &cap, false) !=
            0) {
          free(buf);
          return -1;
        }
        i += 2;
        continue;
      }

      if (isalpha((unsigned char)in[i + 1]) || in[i + 1] == '_') {
        size_t j;

        j = i + 1;
        while (isalnum((unsigned char)in[j]) || in[j] == '_') {
          j++;
        }
        if (append_parameter(in + i + 1, j - (i + 1), state, &buf, &len, &cap,
                             false) != 0) {
          free(buf);
          return -1;
        }
        i = j;
        continue;
      }
    }

    if (in[i] == '`') {
      size_t j;
      char *cmd;
      char *normalized_cmd;
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
      normalized_cmd = normalize_backquote_command_text(cmd);
      free(cmd);
      cmd = normalized_cmd;

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

  if (buf == NULL) {
    buf = arena_xstrdup("");
  }
  restore_quoted_ifs_markers(buf);
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

    size_t ignored;
    bool assignment_word;

    assignment_word = assignment_value_start(in->items[i], &ignored);
    if (expand_token(in->items[i], state, &expanded, false, assignment_word) !=
        0) {
      size_t j;
      for (j = 0; j < out->len; j++) {
        free(out->items[j]);
      }
      free(out->items);
      out->items = NULL;
      out->len = 0;
      return -1;
    }

    if (state->positional_count == 0 &&
        token_is_pure_quoted_at(in->items[i])) {
      free(expanded);
      continue;
    }

    if (expanded_has_at_split_marker(expanded)) {
      (void)append_at_split_expansion(expanded, out, split_fields);
      free(expanded);
      continue;
    }

    /*
     * Unquoted empty expansions are removed from command words.
     * Quoted empties ('' or "") must be preserved.
     */
    if (split_fields && token_has_runtime_expansion(in->items[i])) {
      int count;
      bool had_delim;

      if (expanded[0] == '\0' && token_has_runtime_expansion(in->items[i])) {
        free(expanded);
        continue;
      }
      had_delim = expanded_has_split_delimiter(expanded);
      count = split_and_append_fields(expanded, out);
      if (count > 0) {
        free(expanded);
        continue;
      }
      if (had_delim) {
        free(expanded);
        continue;
      }
    }

    if (split_fields && !state->noglob && token_is_unquoted(in->items[i]) &&
        token_has_glob_meta(expanded)) {
      glob_t g;
      int grc;

      memset(&g, 0, sizeof(g));
      grc = glob(expanded, 0, NULL, &g);
      if (grc == 0 && g.gl_pathc > 0) {
        size_t j;

        for (j = 0; j < g.gl_pathc; j++) {
          out->items = xrealloc(out->items, sizeof(*out->items) * (out->len + 1));
          out->items[out->len++] = arena_xstrdup(g.gl_pathv[j]);
        }
        globfree(&g);
        free(expanded);
        continue;
      }
      if (grc == 0) {
        globfree(&g);
      } else if (grc != GLOB_NOMATCH) {
        globfree(&g);
      }
    }

    restore_quoted_ifs_markers(expanded);
    out->items = xrealloc(out->items, sizeof(*out->items) * (out->len + 1));
    out->items[out->len++] = expanded;
  }

  return 0;
}
