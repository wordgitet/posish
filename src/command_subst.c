/* SPDX-License-Identifier: 0BSD */

/* posish - command substitution helpers */

#include "command_subst.h"

#include "arena.h"
#include "signals.h"

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void cmdsub_append_char(char **buf, size_t *len, size_t *cap, char ch) {
  if (*len + 2 > *cap) {
    size_t new_cap;

    new_cap = *cap == 0 ? 32 : *cap;
    while (*len + 2 > new_cap) {
      new_cap *= 2;
    }
    *buf = arena_xrealloc(*buf, new_cap);
    *cap = new_cap;
  }

  (*buf)[(*len)++] = ch;
  (*buf)[*len] = '\0';
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

bool command_subst_find_close(const char *in, size_t start, size_t *close_out) {
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
    arena_maybe_free(inner_with_candidate);
    arena_maybe_free(inner);

    if (!in_candidate_comment && need_more == 0) {
      *close_out = i;
      return true;
    }
  }

  return false;
}

size_t command_subst_skip_dollar_paren(const char *token, size_t pos) {
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

size_t command_subst_skip_backtick(const char *token, size_t pos) {
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

int command_subst_run(struct shell_state *state, const char *cmd,
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
    arena_init(&local_state.arena_perm, state->arena_perm.default_block_size);
    arena_init(&local_state.arena_script, state->arena_script.default_block_size);
    arena_init(&local_state.arena_cmd, state->arena_cmd.default_block_size);
    arena_set_current(&local_state.arena_perm);
    local_state.should_exit = false;
    local_state.exit_status = 0;
    local_state.exit_trap = NULL;
    local_state.running_signal_trap = false;
    local_state.running_exit_trap = false;
    local_state.main_context = false;
    reset_signal_traps_for_cmdsub(&local_state);

    st = shell_run_command(&local_state, cmd);
    shell_run_pending_traps(&local_state);
    shell_run_exit_trap(&local_state);
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
      buf = arena_xrealloc(buf, cap);
    }

    n = read(pipefd[0], buf + len, cap - len - 1);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      perror("read");
      close(pipefd[0]);
      arena_maybe_free(buf);
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

char *command_subst_normalize_backquote(const char *raw) {
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
          cmdsub_append_char(&out, &len, &cap, next);
        }
        if (next == '"' && quote == '\0') {
          pseudo_dquote = !pseudo_dquote;
        }
        i += 2;
        continue;
      }
    }

    cmdsub_append_char(&out, &len, &cap, raw[i]);
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
