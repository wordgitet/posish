/* SPDX-License-Identifier: 0BSD */

/* posish - simple command execution helpers */

#include "simple_command.h"

#include "arena.h"
#include "builtins/builtin.h"
#include "error.h"
#include "expand.h"
#include "functions.h"
#include "path.h"
#include "spawn.h"
#include "trace.h"
#include "vars.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

struct env_restore {
  char *name;
  char *old_value;
  bool existed;
  bool was_exported;
};

struct env_restore_vec {
  struct env_restore *items;
  size_t len;
};

struct positional_backup {
  char **params;
  size_t count;
};

static void env_restore_vec_free(struct env_restore_vec *restore) {
  size_t i;

  for (i = 0; i < restore->len; i++) {
    arena_maybe_free(restore->items[i].name);
    arena_maybe_free(restore->items[i].old_value);
  }
  arena_maybe_free(restore->items);
  restore->items = NULL;
  restore->len = 0;
}

void simple_command_word_vec_free(struct ast_word_vec *words) {
  arena_maybe_free(words->items);
  words->items = NULL;
  words->len = 0;
}

static void trace_simple_words(struct shell_state *state, char *const words[],
                               size_t count) {
  const char *raw_ps4;
  const char *ps4;
  char *expanded_ps4;
  struct token_vec in;
  struct token_vec out;
  size_t i;

  if (!state->xtrace || count == 0) {
    return;
  }

  raw_ps4 = vars_get(state, "PS4");
  if (raw_ps4 == NULL) {
    raw_ps4 = "+ ";
  }
  ps4 = raw_ps4;
  expanded_ps4 = NULL;

  in.items = (char **)&raw_ps4;
  in.len = 1;
  out.items = NULL;
  out.len = 0;
  if (expand_words(&in, &out, state, false) == 0) {
    if (out.len == 1) {
      expanded_ps4 = out.items[0];
      ps4 = expanded_ps4;
    } else {
      lexer_free_tokens(&out);
    }
  }

  fputs(ps4, stderr);
  for (i = 0; i < count; i++) {
    if (i > 0) {
      fputc(' ', stderr);
    }
    fputs(words[i], stderr);
  }
  fputc('\n', stderr);
  fflush(stderr);
  arena_maybe_free(expanded_ps4);
  arena_maybe_free(out.items);
}

static int word_vec_push(struct ast_word_vec *words, char *word) {
  words->items =
      arena_xrealloc(words->items, sizeof(*words->items) * (words->len + 1));
  words->items[words->len++] = word;
  return 0;
}

static bool has_unsupported_syntax(const char *source) {
  (void)source;
  return false;
}

static bool is_reserved_word_as_command(const char *word) {
  static const char *const reserved[] = {
      "if",   "then", "elif", "else",  "fi",    "do", "done",
      "case", "esac", "for",  "while", "until", "in", "!",
  };
  size_t i;

  for (i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
    if (strcmp(word, reserved[i]) == 0) {
      return true;
    }
  }
  return false;
}

static bool is_name_start_char(char ch) {
  return isalpha((unsigned char)ch) || ch == '_';
}

static bool is_name_char(char ch) {
  return isalnum((unsigned char)ch) || ch == '_';
}

static bool is_assignment_word(const char *word) {
  size_t i;

  if (word[0] == '\0' || !is_name_start_char(word[0])) {
    return false;
  }

  i = 1;
  while (word[i] != '\0' && word[i] != '=') {
    if (!is_name_char(word[i])) {
      return false;
    }
    i++;
  }

  return word[i] == '=';
}

static size_t declaration_utility_prefix_len(
    const struct ast_word_vec *raw_words, size_t assign_count) {
  if (assign_count >= raw_words->len) {
    return 0;
  }

  if (strcmp(raw_words->items[assign_count], "export") == 0 ||
      strcmp(raw_words->items[assign_count], "readonly") == 0) {
    return 1;
  }

  if (assign_count + 2 < raw_words->len &&
      strcmp(raw_words->items[assign_count], "command") == 0 &&
      strcmp(raw_words->items[assign_count + 1], "command") == 0 &&
      (strcmp(raw_words->items[assign_count + 2], "export") == 0 ||
       strcmp(raw_words->items[assign_count + 2], "readonly") == 0)) {
    return 3;
  }

  return 0;
}

static int split_assignment(const char *word, char **name_out,
                            const char **value_out) {
  const char *eq;
  size_t nlen;
  char *name;

  eq = strchr(word, '=');
  if (eq == NULL || eq == word) {
    return -1;
  }

  nlen = (size_t)(eq - word);
  name = arena_xmalloc(nlen + 1);
  memcpy(name, word, nlen);
  name[nlen] = '\0';

  *name_out = name;
  *value_out = eq + 1;
  return 0;
}

static void free_positional_params(char **params, size_t count) {
  size_t i;

  for (i = 0; i < count; i++) {
    arena_maybe_free(params[i]);
  }
  arena_maybe_free(params);
}

static void positional_push(struct shell_state *state, char *const argv[],
                            size_t argc, struct positional_backup *backup) {
  size_t i;

  backup->params = state->positional_params;
  backup->count = state->positional_count;

  state->positional_params = NULL;
  state->positional_count = argc > 0 ? argc - 1 : 0;
  if (state->positional_count == 0) {
    return;
  }

  state->positional_params = arena_alloc_in(
      NULL, sizeof(*state->positional_params) * state->positional_count);
  for (i = 0; i < state->positional_count; i++) {
    state->positional_params[i] = arena_strdup_in(NULL, argv[i + 1]);
  }
}

static void positional_pop(struct shell_state *state,
                           const struct positional_backup *backup) {
  free_positional_params(state->positional_params, state->positional_count);
  state->positional_params = backup->params;
  state->positional_count = backup->count;
}

static bool find_command_subst_end(const char *source, size_t start,
                                   size_t *end_out) {
  size_t i;
  int depth;
  char quote;

  if (source[start] != '$' || source[start + 1] != '(') {
    return false;
  }

  i = start + 2;
  depth = 1;
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
      if (ch == '(') {
        depth++;
        i++;
        continue;
      }
      if (ch == ')') {
        depth--;
        if (depth == 0) {
          *end_out = i;
          return true;
        }
      }
      i++;
      continue;
    }

    if (quote == '\'' && ch == '\'') {
      quote = '\0';
      i++;
      continue;
    }
    if (quote == '"') {
      if (ch == '\\' && source[i + 1] != '\0') {
        i += 2;
        continue;
      }
      if (ch == '"') {
        quote = '\0';
      }
    }
    i++;
  }

  return false;
}

static int find_redir_operator_pos(const char *token, size_t *pos_out) {
  size_t i;
  char quote;

  quote = '\0';
  for (i = 0; token[i] != '\0'; i++) {
    char ch;

    ch = token[i];
    if (quote == '\0') {
      if (ch == '$' && token[i + 1] == '(') {
        size_t end;

        if (find_command_subst_end(token, i, &end)) {
          i = end;
          continue;
        }
      }
      if (ch == '$' && token[i + 1] == '{') {
        size_t j;
        int depth;
        char inner_quote;

        j = i + 2;
        depth = 1;
        inner_quote = '\0';
        while (token[j] != '\0' && depth > 0) {
          char inner;

          inner = token[j];
          if (inner_quote == '\0') {
            if (inner == '\\' && token[j + 1] != '\0') {
              j += 2;
              continue;
            }
            if (inner == '\'' || inner == '"') {
              inner_quote = inner;
              j++;
              continue;
            }
            if (inner == '{') {
              depth++;
            } else if (inner == '}') {
              depth--;
              if (depth == 0) {
                i = j;
                break;
              }
            }
            j++;
            continue;
          }
          if (inner_quote == '\'' && inner == '\'') {
            inner_quote = '\0';
            j++;
            continue;
          }
          if (inner_quote == '"') {
            if (inner == '\\' && token[j + 1] != '\0') {
              j += 2;
              continue;
            }
            if (inner == '"') {
              inner_quote = '\0';
            }
          }
          j++;
        }
        if (depth == 0) {
          continue;
        }
      }
      if (ch == '`') {
        i++;
        while (token[i] != '\0') {
          if (token[i] == '\\' && token[i + 1] != '\0') {
            i += 2;
            continue;
          }
          if (token[i] == '`') {
            break;
          }
          i++;
        }
        if (token[i] == '\0') {
          return -1;
        }
        continue;
      }
      if (ch == '\\' && token[i + 1] != '\0') {
        i++;
        continue;
      }
      if (ch == '\'' || ch == '"') {
        quote = ch;
        continue;
      }
      if (ch == '<' || ch == '>') {
        *pos_out = i;
        return 0;
      }
      continue;
    }

    if (quote == '\'' && ch == '\'') {
      quote = '\0';
    } else if (quote == '"') {
      if (ch == '\\' && token[i + 1] != '\0') {
        i++;
        continue;
      }
      if (ch == '"') {
        quote = '\0';
      }
    }
  }

  return -1;
}

int simple_command_collect_words_and_redirs(const struct token_vec *expanded,
                                            struct ast_word_vec *words,
                                            struct redir_vec *redirs) {
  size_t i;

  words->items = NULL;
  words->len = 0;
  redirs->items = NULL;
  redirs->len = 0;

  for (i = 0; i < expanded->len; i++) {
    struct redir_spec spec;
    bool needs_word;
    int pr;

    pr = parse_redir_token(expanded->items[i], &spec, &needs_word);
    if (pr < 0) {
      return -1;
    }
    if (pr == 0) {
      size_t op_pos;

      if (find_redir_operator_pos(expanded->items[i], &op_pos) == 0 &&
          op_pos > 0) {
        char *prefix;
        const char *redir_text;

        prefix = arena_xmalloc(op_pos + 1);
        memcpy(prefix, expanded->items[i], op_pos);
        prefix[op_pos] = '\0';
        word_vec_push(words, prefix);

        redir_text = expanded->items[i] + op_pos;
        pr = parse_redir_token(redir_text, &spec, &needs_word);
        if (pr < 0) {
          return -1;
        }
        if (pr == 0) {
          word_vec_push(words, expanded->items[i]);
          continue;
        }
      } else {
        word_vec_push(words, expanded->items[i]);
        continue;
      }
    }

    if (needs_word) {
      i++;
      if (i >= expanded->len) {
        posish_error_idf(POSERR_MISSING_REDIRECTION_OPERAND);
        return -1;
      }

      spec.path = arena_xstrdup(expanded->items[i]);
    }

    redir_vec_push(redirs, &spec);
  }

  return 0;
}

static int apply_persistent_assignments(struct shell_state *state,
                                        char *const words[], size_t count) {
  size_t i;

  for (i = 0; i < count; i++) {
    char *name;
    const char *value;

    if (split_assignment(words[i], &name, &value) != 0) {
      continue;
    }

    if (vars_set_assignment(state, name, value, true) != 0) {
      if (!state->interactive) {
        state->should_exit = true;
        state->exit_status = 1;
      }
      arena_maybe_free(name);
      return 1;
    }
    arena_maybe_free(name);
  }

  return 0;
}

static void restore_temporary_assignments(struct shell_state *state,
                                          struct env_restore_vec *restore) {
  size_t i;

  for (i = restore->len; i > 0; i--) {
    struct env_restore *r;

    r = &restore->items[i - 1];
    if (r->existed) {
      if (vars_set_with_mode(state, r->name, r->old_value, false,
                             r->was_exported) != 0) {
        perror("setenv");
      }
    } else {
      if (vars_unset(state, r->name) != 0) {
        perror("unsetenv");
      }
    }
  }
}

static int apply_temporary_assignments(struct shell_state *state,
                                       char *const words[], size_t count,
                                       struct env_restore_vec *restore) {
  size_t i;

  restore->items = NULL;
  restore->len = 0;

  for (i = 0; i < count; i++) {
    struct env_restore r;
    char *name;
    const char *value;
    const char *old;

    if (split_assignment(words[i], &name, &value) != 0) {
      continue;
    }

    r.name = name;
    r.old_value = NULL;
    r.existed = false;
    r.was_exported = false;

    old = vars_get(state, name);
    if (vars_is_set(state, name)) {
      r.old_value = arena_xstrdup(old);
      r.existed = true;
      r.was_exported = vars_is_exported(state, name);
    }

    if (vars_set(state, name, value, true) != 0) {
      if (!state->interactive) {
        state->should_exit = true;
        state->exit_status = 1;
      }
      arena_maybe_free(r.name);
      arena_maybe_free(r.old_value);
      restore_temporary_assignments(state, restore);
      env_restore_vec_free(restore);
      return 1;
    }

    restore->items = arena_xrealloc(restore->items,
                                    sizeof(*restore->items) *
                                        (restore->len + 1));
    restore->items[restore->len++] = r;
  }

  return 0;
}

static bool is_exec_without_command(char *const argv[]) {
  size_t i;

  if (strcmp(argv[0], "exec") != 0) {
    return false;
  }

  i = 1;
  if (argv[i] != NULL && strcmp(argv[i], "--") == 0) {
    i++;
  }

  return argv[i] == NULL;
}

static bool is_command_exec_without_command(char *const argv[]) {
  size_t i;
  bool opt_v;
  bool opt_V;

  if (strcmp(argv[0], "command") != 0) {
    return false;
  }

  i = 1;
  opt_v = false;
  opt_V = false;
  while (argv[i] != NULL) {
    size_t j;

    if (strcmp(argv[i], "--") == 0) {
      i++;
      break;
    }
    if (argv[i][0] != '-' || argv[i][1] == '\0') {
      break;
    }
    for (j = 1; argv[i][j] != '\0'; j++) {
      if (argv[i][j] == 'v') {
        opt_v = true;
        opt_V = false;
        continue;
      }
      if (argv[i][j] == 'V') {
        opt_V = true;
        opt_v = false;
        continue;
      }
      if (argv[i][j] == 'p') {
        continue;
      }
      return false;
    }
    i++;
  }

  if (opt_v || opt_V) {
    return false;
  }
  if (argv[i] == NULL || strcmp(argv[i], "exec") != 0) {
    return false;
  }
  i++;
  if (argv[i] != NULL && strcmp(argv[i], "--") == 0) {
    i++;
  }
  return argv[i] == NULL;
}

int simple_command_parse_redirections_from_source(const char *source,
                                                  struct shell_state *state,
                                                  struct redir_vec *redirs) {
  struct token_vec lexed;
  struct token_vec expanded;
  struct ast_word_vec words;
  int rc;

  redirs->items = NULL;
  redirs->len = 0;

  if (source[0] == '\0') {
    return 0;
  }

  if (lexer_split_words(source, &lexed) != 0) {
    return -1;
  }
  if (expand_words(&lexed, &expanded, state, false) != 0) {
    lexer_free_tokens(&lexed);
    return -1;
  }
  lexer_free_tokens(&lexed);

  rc = simple_command_collect_words_and_redirs(&expanded, &words, redirs);
  if (rc != 0) {
    lexer_free_tokens(&expanded);
    return -1;
  }
  if (redir_expand_operands(state, redirs, NULL, NULL) != 0) {
    simple_command_word_vec_free(&words);
    redir_vec_free(redirs);
    lexer_free_tokens(&expanded);
    return -1;
  }

  if (words.len != 0) {
    posish_error_idf(POSERR_UNSUPPORTED_TOKENS_AFTER_GROUP);
    simple_command_word_vec_free(&words);
    redir_vec_free(redirs);
    lexer_free_tokens(&expanded);
    return -1;
  }

  simple_command_word_vec_free(&words);
  lexer_free_tokens(&expanded);
  return 0;
}

int simple_command_execute_parts(struct shell_state *state,
                                 const struct ast_word_vec *ast_raw_words,
                                 const struct redir_vec *ast_redirs,
                                 bool allow_builtin,
                                 simple_command_body_runner run_body) {
  struct ast_word_vec raw_words;
  struct token_vec expanded;
  struct token_vec assign_expanded;
  struct token_vec cmd_expanded;
  struct ast_word_vec words;
  struct redir_vec redirs;
  struct env_restore_vec temp_env;
  struct fd_backup_vec fd_backups;
  struct fd_backup_vec pre_expand_backups;
  char **argv;
  size_t i;
  size_t assign_count;
  size_t argc;
  int status;
  bool handled;
  bool have_temp_env;
  bool special_name;
  bool assignment_special;
  bool persist_builtin_redirs;
  const struct shell_function *function_def;
  struct positional_backup positional_backup;
  bool saw_cmdsub;
  int last_cmdsub_status;
  bool pre_expand_redirs;
  bool redirs_only_command;
  struct token_vec in_vec;

  raw_words.items = ast_raw_words->items;
  raw_words.len = ast_raw_words->len;
  expanded.items = NULL;
  expanded.len = 0;
  assign_expanded.items = NULL;
  assign_expanded.len = 0;
  cmd_expanded.items = NULL;
  cmd_expanded.len = 0;
  words.items = NULL;
  words.len = 0;
  redirs.items = NULL;
  redirs.len = 0;
  temp_env.items = NULL;
  temp_env.len = 0;
  fd_backups.items = NULL;
  fd_backups.len = 0;
  pre_expand_backups.items = NULL;
  pre_expand_backups.len = 0;
  argv = NULL;
  argc = 0;
  status = 0;
  handled = false;
  have_temp_env = false;
  function_def = NULL;
  saw_cmdsub = false;
  last_cmdsub_status = 0;
  pre_expand_redirs = false;
  redirs_only_command = false;
  assignment_special = false;

  redir_vec_clone(&redirs, ast_redirs);

  redirs_only_command = raw_words.len == 0;

  assign_count = 0;
  while (assign_count < raw_words.len &&
         is_assignment_word(raw_words.items[assign_count])) {
    assign_count++;
  }

  in_vec.items = raw_words.items + assign_count;
  in_vec.len = raw_words.len - assign_count;
  if (in_vec.len > 0 && is_reserved_word_as_command(in_vec.items[0])) {
    posish_error_idf(POSERR_UNEXPECTED_TOKEN, in_vec.items[0]);
    if (!state->interactive) {
      state->should_exit = true;
      state->exit_status = 2;
    }
    status = 2;
    goto done;
  }
  if (in_vec.len > 0) {
    size_t decl_prefix_len;
    size_t wi;

    decl_prefix_len = declaration_utility_prefix_len(&raw_words, assign_count);
    for (wi = 0; wi < in_vec.len; wi++) {
      struct token_vec one_in;
      struct token_vec one_out;
      bool split_fields;
      size_t oi;

      one_in.items = &in_vec.items[wi];
      one_in.len = 1;
      one_out.items = NULL;
      one_out.len = 0;

      split_fields = true;
      if (decl_prefix_len > 0 && wi >= decl_prefix_len &&
          is_assignment_word(in_vec.items[wi])) {
        split_fields = false;
      }

      if (expand_words(&one_in, &one_out, state, split_fields) != 0) {
        status = 2;
        goto done;
      }
      if (state->cmdsub_performed) {
        saw_cmdsub = true;
        last_cmdsub_status = state->last_cmdsub_status;
      }
      if (one_out.len > 0) {
        cmd_expanded.items = arena_xrealloc(
            cmd_expanded.items,
            sizeof(*cmd_expanded.items) * (cmd_expanded.len + one_out.len));
        for (oi = 0; oi < one_out.len; oi++) {
          cmd_expanded.items[cmd_expanded.len++] = one_out.items[oi];
        }
      }
      arena_maybe_free(one_out.items);
    }
  }

  special_name = allow_builtin && cmd_expanded.len > 0 &&
                 cmd_expanded.items[0][0] != '\0' &&
                 builtin_is_special_name(cmd_expanded.items[0]);
  assignment_special =
      special_name && strcmp(cmd_expanded.items[0], "command") != 0;

  if (!redirs_only_command) {
    bool redir_saw_cmdsub;
    int redir_last_cmdsub_status;

    redir_saw_cmdsub = false;
    redir_last_cmdsub_status = 0;
    status = redir_expand_operands(state, &redirs, &redir_saw_cmdsub,
                                   &redir_last_cmdsub_status);
    if (redir_saw_cmdsub) {
      saw_cmdsub = true;
      last_cmdsub_status = redir_last_cmdsub_status;
    }
    if (status != 0) {
      goto done;
    }
  }

  if (assign_count > 0 && cmd_expanded.len > 0 && !assignment_special) {
    if (apply_redirections(state, &redirs, true, state->noclobber, false,
                           &pre_expand_backups) != 0) {
      status = 1;
      goto done;
    }
    pre_expand_redirs = true;
  }

  in_vec.items = raw_words.items;
  in_vec.len = assign_count;
  if (in_vec.len > 0) {
    if (expand_words(&in_vec, &assign_expanded, state, false) != 0) {
      status = 2;
      goto done;
    }
    if (state->cmdsub_performed) {
      saw_cmdsub = true;
      last_cmdsub_status = state->last_cmdsub_status;
    }
  }

  if (pre_expand_redirs) {
    fd_backup_restore(&pre_expand_backups);
    pre_expand_redirs = false;
  }

  state->cmdsub_performed = saw_cmdsub;
  state->last_cmdsub_status = saw_cmdsub ? last_cmdsub_status : 0;

  if (redirs.len == 0 && raw_words.len == 1 && assign_count == 1 &&
      cmd_expanded.len == 0 && assign_expanded.len == 1 &&
      is_assignment_word(assign_expanded.items[0])) {
    char *assign_words[1];

    assign_words[0] = assign_expanded.items[0];
    trace_simple_words(state, assign_words, 1);
    status = apply_persistent_assignments(state, assign_words, 1);
    simple_command_word_vec_free(&raw_words);
    lexer_free_tokens(&assign_expanded);
    lexer_free_tokens(&cmd_expanded);
    redir_vec_free(&redirs);
    if (status == 0 && state->cmdsub_performed) {
      return state->last_cmdsub_status;
    }
    return status;
  }

  if (assign_expanded.len == 0) {
    words.items = cmd_expanded.items;
    words.len = cmd_expanded.len;
    cmd_expanded.items = NULL;
    cmd_expanded.len = 0;
  } else if (cmd_expanded.len == 0) {
    words.items = assign_expanded.items;
    words.len = assign_expanded.len;
    assign_expanded.items = NULL;
    assign_expanded.len = 0;
  } else {
    expanded.len = assign_expanded.len + cmd_expanded.len;
    expanded.items = arena_xmalloc(sizeof(*expanded.items) * expanded.len);
    for (i = 0; i < assign_expanded.len; i++) {
      expanded.items[i] = assign_expanded.items[i];
    }
    for (i = 0; i < cmd_expanded.len; i++) {
      expanded.items[assign_expanded.len + i] = cmd_expanded.items[i];
    }
    words.items = expanded.items;
    words.len = expanded.len;
    expanded.items = NULL;
    expanded.len = 0;
  }
  arena_maybe_free(assign_expanded.items);
  assign_expanded.items = NULL;
  assign_expanded.len = 0;
  arena_maybe_free(cmd_expanded.items);
  cmd_expanded.items = NULL;
  cmd_expanded.len = 0;

  simple_command_word_vec_free(&raw_words);

  if (words.len == 0) {
    if (redirs_only_command) {
      pid_t pid;
      int wstatus;

      pid = fork();
      if (pid < 0) {
        perror("fork");
        redir_vec_free(&redirs);
        simple_command_word_vec_free(&words);
        lexer_free_tokens(&expanded);
        return 1;
      }
      if (pid == 0) {
        struct shell_state local_state;
        bool redir_saw_cmdsub;
        int redir_last_cmdsub_status;
        int st;

        local_state = *state;
        arena_init(&local_state.arena_perm,
                   state->arena_perm.default_block_size);
        arena_init(&local_state.arena_script,
                   state->arena_script.default_block_size);
        arena_init(&local_state.arena_cmd, state->arena_cmd.default_block_size);
        arena_set_current(&local_state.arena_perm);
        local_state.should_exit = false;
        local_state.exit_status = 0;
        local_state.running_signal_trap = false;
        local_state.running_exit_trap = false;
        local_state.main_context = false;
        redir_saw_cmdsub = false;
        redir_last_cmdsub_status = 0;

        st = redir_expand_operands(&local_state, &redirs, &redir_saw_cmdsub,
                                   &redir_last_cmdsub_status);
        if (st != 0) {
          _exit(st);
        }
        if (redir_saw_cmdsub) {
          local_state.cmdsub_performed = true;
          local_state.last_cmdsub_status = redir_last_cmdsub_status;
        }
        if (apply_redirections(&local_state, &redirs, false,
                               local_state.noclobber, false, NULL) != 0) {
          _exit(1);
        }
        st = local_state.cmdsub_performed ? local_state.last_cmdsub_status : 0;
        _exit(st);
      }

      for (;;) {
        if (waitpid(pid, &wstatus, 0) < 0) {
          if (errno == EINTR) {
            shell_run_pending_traps(state);
            continue;
          }
          perror("waitpid");
          redir_vec_free(&redirs);
          simple_command_word_vec_free(&words);
          lexer_free_tokens(&expanded);
          return 1;
        }
        break;
      }

      redir_vec_free(&redirs);
      simple_command_word_vec_free(&words);
      lexer_free_tokens(&expanded);
      if (WIFEXITED(wstatus)) {
        return WEXITSTATUS(wstatus);
      }
      if (WIFSIGNALED(wstatus)) {
        return shell_status_from_wait_status(wstatus);
      }
      return 1;
    }

    if (apply_redirections(state, &redirs, true, state->noclobber, false,
                           &fd_backups) != 0) {
      fd_backup_restore(&fd_backups);
      redir_vec_free(&redirs);
      simple_command_word_vec_free(&words);
      lexer_free_tokens(&expanded);
      return 1;
    }
    fflush(NULL);
    fd_backup_restore(&fd_backups);
    redir_vec_free(&redirs);
    simple_command_word_vec_free(&words);
    lexer_free_tokens(&expanded);
    if (state->cmdsub_performed) {
      return state->last_cmdsub_status;
    }
    return 0;
  }

  trace_simple_words(state, words.items, words.len);

  assign_count = 0;
  while (assign_count < words.len &&
         is_assignment_word(words.items[assign_count])) {
    assign_count++;
  }

  if (assign_count == words.len) {
    status = apply_persistent_assignments(state, words.items, assign_count);
    if (status == 0) {
      if (apply_redirections(state, &redirs, true, state->noclobber, false,
                             &fd_backups) != 0) {
        status = 1;
      }
    }
    fflush(NULL);
    fd_backup_restore(&fd_backups);

    redir_vec_free(&redirs);
    simple_command_word_vec_free(&words);
    lexer_free_tokens(&expanded);
    if (status == 0 && state->cmdsub_performed) {
      return state->last_cmdsub_status;
    }
    return status;
  }

  argc = words.len - assign_count;
  argv = arena_xmalloc(sizeof(*argv) * (argc + 1));
  for (i = 0; i < argc; i++) {
    argv[i] = words.items[assign_count + i];
  }
  argv[argc] = NULL;
  if (argc == 1 && argv[0][0] == '\0') {
    status = apply_persistent_assignments(state, words.items, assign_count);
    if (status == 0) {
      if (apply_redirections(state, &redirs, true, state->noclobber, false,
                             &fd_backups) != 0) {
        status = 1;
      }
    }
    fflush(NULL);
    fd_backup_restore(&fd_backups);
    arena_maybe_free(argv);
    redir_vec_free(&redirs);
    simple_command_word_vec_free(&words);
    lexer_free_tokens(&expanded);
    if (status == 0 && state->cmdsub_performed) {
      return state->last_cmdsub_status;
    }
    return status;
  }

  special_name = allow_builtin && builtin_is_special_name(argv[0]);
  function_def =
      allow_builtin && !special_name ? functions_get(state, argv[0]) : NULL;
  assignment_special = special_name && strcmp(argv[0], "command") != 0;
  persist_builtin_redirs =
      (special_name && is_exec_without_command(argv)) ||
      (allow_builtin && is_command_exec_without_command(argv));

  if (allow_builtin && assign_count == 0 && redirs.len == 0 &&
      function_def == NULL) {
    bool builtin_available;

    builtin_available = builtin_is_name(argv[0]) &&
                        (!builtin_is_substitutive_name(argv[0]) ||
                         path_resolves_command(state, argv[0], false));
    if (builtin_available) {
      status = builtin_dispatch(state, argv, &handled);
      if (handled) {
        goto done;
      }
    }
  }

  if (assign_count > 0) {
    if (assignment_special) {
      status = apply_persistent_assignments(state, words.items, assign_count);
      if (status != 0) {
        goto done;
      }
    } else {
      if (apply_temporary_assignments(state, words.items, assign_count,
                                      &temp_env) != 0) {
        status = 1;
        goto done;
      }
      have_temp_env = true;
    }
  }

  if (allow_builtin) {
    bool run_in_shell;
    bool builtin_available;

    builtin_available = builtin_is_name(argv[0]) &&
                        (!builtin_is_substitutive_name(argv[0]) ||
                         path_resolves_command(state, argv[0], false));
    run_in_shell = function_def != NULL || builtin_available;
    if (persist_builtin_redirs) {
      if (apply_redirections(state, &redirs, false, state->noclobber, false,
                             NULL) != 0) {
        status = 1;
        if (special_name && !state->interactive) {
          state->should_exit = true;
          state->exit_status = status;
        }
        goto done;
      }

      status = builtin_dispatch(state, argv, &handled);
      if (!handled) {
        status = spawn_run_external_argv(state, argv, &redirs);
      }
    } else if (run_in_shell) {
      if (apply_redirections(state, &redirs, true, state->noclobber, false,
                             &fd_backups) != 0) {
        status = 1;
        if (special_name && !state->interactive) {
          state->should_exit = true;
          state->exit_status = status;
        }
        fd_backup_restore(&fd_backups);
        goto done;
      }

      if (function_def != NULL) {
        struct fd_backup_vec function_backups;
        struct redir_vec function_redirs;
        bool function_redirs_applied;

        positional_push(state, argv, argc, &positional_backup);
        state->function_depth++;
        function_backups.items = NULL;
        function_backups.len = 0;
        function_redirs.items = NULL;
        function_redirs.len = 0;
        function_redirs_applied = false;
        if (function_def->redirs.len > 0) {
          if (prepare_runtime_redirections(state, &function_def->redirs,
                                           &function_redirs) != 0 ||
              apply_redirections(state, &function_redirs, true,
                                 state->noclobber, false,
                                 &function_backups) != 0) {
            status = 1;
          } else {
            function_redirs_applied = true;
          }
        }
        if (status == 0) {
          status = run_body(state, function_def->body);
        }
        if (function_redirs_applied) {
          fd_backup_restore(&function_backups);
        }
        redir_vec_free(&function_redirs);
        state->function_depth--;
        positional_pop(state, &positional_backup);
        if (state->return_requested) {
          status = state->return_status;
          state->return_requested = false;
        }
        handled = true;
      } else {
        status = builtin_dispatch(state, argv, &handled);
      }
      fflush(NULL);
      fd_backup_restore(&fd_backups);

      if (!handled) {
        status = spawn_run_external_argv(state, argv, &redirs);
      }
    } else {
      status = spawn_run_external_argv(state, argv, &redirs);
    }
  } else {
    status = spawn_run_external_argv(state, argv, &redirs);
  }

done:
  if (pre_expand_redirs) {
    fd_backup_restore(&pre_expand_backups);
  }
  if (have_temp_env) {
    restore_temporary_assignments(state, &temp_env);
    env_restore_vec_free(&temp_env);
  }

  if (status == 0 && state->cmdsub_performed && argc == 1 &&
      argv != NULL && argv[0][0] == '\0') {
    status = state->last_cmdsub_status;
  }

  arena_maybe_free(argv);
  redir_vec_free(&redirs);
  simple_command_word_vec_free(&words);
  simple_command_word_vec_free(&raw_words);
  lexer_free_tokens(&expanded);
  lexer_free_tokens(&assign_expanded);
  lexer_free_tokens(&cmd_expanded);
  return status;
}

int simple_command_execute(struct shell_state *state, const char *source,
                           bool allow_builtin,
                           simple_command_body_runner run_body) {
  struct token_vec lexed;
  struct ast_word_vec raw_words;
  struct redir_vec redirs;
  int status;

  lexed.items = NULL;
  lexed.len = 0;
  raw_words.items = NULL;
  raw_words.len = 0;
  redirs.items = NULL;
  redirs.len = 0;

  if (has_unsupported_syntax(source)) {
    posish_error_idf(POSERR_COMPLEX_SYNTAX_UNIMPLEMENTED);
    return 2;
  }

  if (lexer_split_words_at(state->current_source_name, source,
                           state->current_source_base_line, &lexed) != 0) {
    return 2;
  }

  if (simple_command_collect_words_and_redirs(&lexed, &raw_words, &redirs) !=
      0) {
    lexer_free_tokens(&lexed);
    return 2;
  }
  lexer_free_tokens(&lexed);

  status =
      simple_command_execute_parts(state, &raw_words, &redirs, allow_builtin,
                                   run_body);
  return status;
}
