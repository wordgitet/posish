/* SPDX-License-Identifier: 0BSD */

/* posish - execution engine */

#include "exec.h"

#include "alias.h"
#include "arena.h"
#include "ast_exec.h"
#include "builtins/builtin.h"
#include "case_command.h"
#include "compound_parse.h"
#include "error.h"
#include "expand.h"
#include "functions.h"
#include "jobs.h"
#include "lexer.h"
#include "parser.h"
#include "path.h"
#include "program_text.h"
#include "redir.h"
#include "signals.h"
#include "simple_command.h"
#include "spawn.h"
#include "trace.h"
#include "vars.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int execute_program_text(struct shell_state *state, const char *source);
static int execute_program_text_internal(struct shell_state *state,
                                         const char *source,
                                         bool apply_aliases);
static int execute_ast_node(struct shell_state *state,
                            const struct ast_node *node, bool allow_builtin);
static int execute_ast_pipeline(struct shell_state *state,
                                const struct ast_node *node);
static int run_for_ast(struct shell_state *state, const struct ast_node *node);
static int run_function_def_ast(struct shell_state *state,
                                const struct ast_node *node);
static int run_case_ast(struct shell_state *state, const struct ast_node *node);
static int run_simple_ast(struct shell_state *state,
                          const struct ast_node *node, bool allow_builtin);
static void maybe_trigger_errexit(struct shell_state *state, int status);
static int execute_command_atom(struct shell_state *state, const char *source,
                                bool allow_builtin);
static int run_program_text_child_body(struct shell_state *state,
                                       const void *payload);
static bool parse_function_definition(const char *source, char **name_out,
                                      char **body_out);
static bool keyword_boundary(char ch);
static bool unwrap_subshell_group(const char *source, char **inner_out,
                                  char **redir_suffix_out);
static bool unwrap_brace_group(const char *source, char **inner_out,
                               char **redir_suffix_out);
static bool split_case_redirection_suffix(const char *source, char **core_out,
                                          char **suffix_out);
static bool ast_node_is_ast_owned(const struct ast_node *node);
static bool try_run_ast_compound_command(struct shell_state *state,
                                         const char *source, bool allow_builtin,
                                         int *status_out);
static void exec_child_command(struct shell_state *parent_state,
                               const char *source);
static const struct program_text_hooks *get_program_text_hooks(void);

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

static char *dup_slice(const char *src, size_t start, size_t end) {
  char *out;
  size_t len;

  len = end - start;
  out = arena_xmalloc(len + 1);
  if (len > 0) {
    memcpy(out, src + start, len);
  }
  out[len] = '\0';
  return out;
}

static void set_lineno_for_ast_node(struct shell_state *state,
                                    const struct ast_node *node) {
  char line_buf[32];

  if (node == NULL || node->span.start_line == 0) {
    return;
  }

  snprintf(line_buf, sizeof(line_buf), "%zu", node->span.start_line);
  (void)vars_set_with_mode(state, "LINENO", line_buf, false, false);
}

static bool ast_node_is_ast_owned(const struct ast_node *node) {
  return node != NULL && node->kind != AST_NODE_LEGACY;
}

static bool try_run_ast_compound_command(struct shell_state *state,
                                         const char *source, bool allow_builtin,
                                         int *status_out) {
  struct ast_program *program;

  program = NULL;
  if (parse_program_at(state->current_source_name,
                       state->current_source_base_line, source,
                       &program) != 0) {
    *status_out = 2;
    return true;
  }
  if (program == NULL || program->root == NULL) {
    *status_out = 0;
    return true;
  }
  if (!ast_node_is_ast_owned(program->root)) {
    return false;
  }

  *status_out = execute_ast_node(state, program->root, allow_builtin);
  return true;
}

bool exec_noexec_allows_set_toggle(const char *source) {
  struct token_vec tokens;
  size_t i;
  bool allowed;

  tokens.items = NULL;
  tokens.len = 0;
  if (lexer_split_words(source, &tokens) != 0) {
    return false;
  }

  allowed = false;
  if (tokens.len == 0 || strcmp(tokens.items[0], "set") != 0) {
    lexer_free_tokens(&tokens);
    return false;
  }

  for (i = 1; i < tokens.len; i++) {
    const char *opt;
    size_t j;

    opt = tokens.items[i];
    if (strcmp(opt, "--") == 0) {
      break;
    }
    if (strcmp(opt, "+o") == 0) {
      if (i + 1 < tokens.len && strcmp(tokens.items[i + 1], "noexec") == 0) {
        allowed = true;
      }
      break;
    }
    if (strncmp(opt, "+o", 2) == 0 && strcmp(opt + 2, "noexec") == 0) {
      allowed = true;
      break;
    }
    if (opt[0] != '+' && opt[0] != '-') {
      break;
    }
    if (opt[0] != '+') {
      continue;
    }
    for (j = 1; opt[j] != '\0'; j++) {
      if (opt[j] == 'n') {
        allowed = true;
        break;
      }
    }
    if (allowed) {
      break;
    }
  }

  lexer_free_tokens(&tokens);
  return allowed;
}

static bool is_name_start_char(char ch) {
  return isalpha((unsigned char)ch) || ch == '_';
}

static bool is_name_char(char ch) {
  return isalnum((unsigned char)ch) || ch == '_';
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
        ch == ')' || ch == '{' || ch == '}') {
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
        (len == 4 && strncmp(source + start, "elif", 4) == 0) ||
        (len == 2 && strncmp(source + start, "if", 2) == 0) ||
        (len == 2 && strncmp(source + start, "fi", 2) == 0)) {
      return true;
    }
  }

  return false;
}

static size_t skip_continuations_forward(const char *source, size_t pos) {
  while (source[pos] == '\\' && source[pos + 1] == '\n') {
    pos += 2;
  }
  return pos;
}

static bool parse_alt_parameter_command(const char *source, char **name_out,
                                        char **word_out) {
  size_t len;
  size_t i;
  size_t name_len;
  size_t word_start;

  *name_out = NULL;
  *word_out = NULL;

  len = strlen(source);
  if (len < 4 || source[0] != '$' || source[1] != '{' ||
      source[len - 1] != '}') {
    return false;
  }

  i = 2;
  while (i + 2 < len) {
    if (source[i] == ':' && source[i + 1] == '+') {
      break;
    }
    i++;
  }
  if (i + 2 >= len || source[i] != ':' || source[i + 1] != '+') {
    return false;
  }

  name_len = i - 2;
  if (name_len == 0) {
    return false;
  }
  if (!is_name_start_char(source[2])) {
    return false;
  }
  for (i = 3; i < 2 + name_len; i++) {
    if (!is_name_char(source[i])) {
      return false;
    }
  }

  word_start = 2 + name_len + 2;
  *name_out = dup_trimmed_slice(source, 2, 2 + name_len);
  *word_out = dup_trimmed_slice(source, word_start, len - 1);
  return true;
}

static bool try_execute_alt_parameter_command(struct shell_state *state,
                                              const char *source,
                                              int *status_out) {
  char *name;
  char *word;
  const char *value;

  if (!parse_alt_parameter_command(source, &name, &word)) {
    return false;
  }

  value = vars_get(state, name);
  if (value != NULL && value[0] != '\0' && word[0] != '\0') {
    *status_out = program_text_execute(state, word, get_program_text_hooks());
  } else {
    *status_out = 0;
  }

  arena_maybe_free(name);
  arena_maybe_free(word);
  return true;
}

static bool parse_function_definition(const char *source, char **name_out,
                                      char **body_out) {
  size_t i;
  size_t name_start;
  size_t name_end;
  size_t body_start;
  size_t body_end;

  i = 0;
  while (isspace((unsigned char)source[i])) {
    i++;
  }
  name_start = i;
  if (!is_name_start_char(source[i])) {
    return false;
  }
  i++;
  while (is_name_char(source[i])) {
    i++;
  }
  name_end = i;

  while (isspace((unsigned char)source[i])) {
    i++;
  }
  if (source[i] != '(') {
    return false;
  }
  i++;
  while (isspace((unsigned char)source[i])) {
    i++;
  }
  if (source[i] != ')') {
    return false;
  }
  i++;
  while (isspace((unsigned char)source[i])) {
    i++;
  }
  if (source[i] == '\0') {
    return false;
  }

  /*
   * POSIX allows any compound command as a function body, not just brace
   * groups. Keep the full trailing command text as the function body.
   */
  body_start = i;
  body_end = strlen(source);
  while (body_end > body_start &&
         isspace((unsigned char)source[body_end - 1])) {
    body_end--;
  }
  if (body_end <= body_start) {
    return false;
  }

  *name_out = dup_trimmed_slice(source, name_start, name_end);
  *body_out = dup_trimmed_slice(source, body_start, body_end);
  return true;
}

static bool has_pending_flow_control(const struct shell_state *state) {
  return state->break_levels > 0 || state->continue_levels > 0 ||
         state->return_requested;
}


static bool unwrap_subshell_group(const char *source, char **inner_out,
                                  char **redir_suffix_out) {
  size_t len;
  size_t i;
  int paren_depth;
  char quote;
  size_t close_pos;

  len = strlen(source);
  if (len < 2 || source[0] != '(') {
    return false;
  }

  quote = '\0';
  paren_depth = 0;
  close_pos = (size_t)-1;
  for (i = 0; i < len; i++) {
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
      if (ch == '(') {
        paren_depth++;
      } else if (ch == ')') {
        paren_depth--;
        if (paren_depth < 0) {
          return false;
        }
        if (paren_depth == 0) {
          close_pos = i;
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

  if (quote != '\0' || paren_depth != 0 || close_pos == (size_t)-1) {
    return false;
  }

  *inner_out = dup_trimmed_slice(source, 1, close_pos);
  *redir_suffix_out = dup_trimmed_slice(source, close_pos + 1, len);
  return true;
}

static bool unwrap_brace_group(const char *source, char **inner_out,
                               char **redir_suffix_out) {
  size_t len;
  size_t i;
  int brace_depth;
  char quote;
  size_t close_pos;

  len = strlen(source);
  if (len < 2 || source[0] != '{') {
    return false;
  }

  quote = '\0';
  brace_depth = 0;
  close_pos = (size_t)-1;
  for (i = 0; i < len; i++) {
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

      if (ch == '{') {
        brace_depth++;
      } else if (ch == '}') {
        brace_depth--;
        if (brace_depth == 0) {
          close_pos = i;
          break;
        }
        if (brace_depth < 0) {
          return false;
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

  if (quote != '\0' || brace_depth != 0 || close_pos == (size_t)-1) {
    return false;
  }

  *inner_out = dup_trimmed_slice(source, 1, close_pos);
  *redir_suffix_out = dup_trimmed_slice(source, close_pos + 1, len);
  return true;
}

static int run_subshell_command(struct shell_state *parent_state,
                                const char *source) {
  return spawn_run_subshell_payload(parent_state, source, source,
                                    run_program_text_child_body);
}

static int run_group_with_redirection_source(struct shell_state *state,
                                             const char *body,
                                             const char *redir_source,
                                             ast_exec_body_runner run_body) {
  struct redir_vec redirs;
  int status;

  if (redir_source == NULL || redir_source[0] == '\0') {
    return run_body(state, body);
  }

  redirs.items = NULL;
  redirs.len = 0;
  if (simple_command_parse_redirections_from_source(redir_source, state,
                                                    &redirs) != 0) {
    redir_vec_free(&redirs);
    return 2;
  }

  status = ast_exec_run_group_with_redirections(state, body, &redirs, run_body);
  redir_vec_free(&redirs);
  return status;
}

static int run_async_list(struct shell_state *state, const char *source) {
  return spawn_run_async_payload(state, source, source,
                                 run_program_text_child_body);
}

static int run_for_ast(struct shell_state *state, const struct ast_node *node) {
  struct token_vec for_lexed;
  struct ast_word_vec for_raw_words;
  struct redir_vec for_redirs;
  struct token_vec for_expanded;
  struct token_vec for_in;
  struct fd_backup_vec for_backups;
  struct redir_vec runtime_redirs;
  bool for_redir_applied;
  size_t i;
  int status;

  for_lexed.items = NULL;
  for_lexed.len = 0;
  for_raw_words.items = NULL;
  for_raw_words.len = 0;
  for_redirs.items = NULL;
  for_redirs.len = 0;
  for_expanded.items = NULL;
  for_expanded.len = 0;
  for_backups.items = NULL;
  for_backups.len = 0;
  runtime_redirs.items = NULL;
  runtime_redirs.len = 0;
  for_redir_applied = false;
  status = 0;

  if (node->data.for_cmd.redirs.len != 0) {
    if (prepare_runtime_redirections(state, &node->data.for_cmd.redirs,
                                     &runtime_redirs) != 0 ||
        apply_redirections(state, &runtime_redirs, true, state->noclobber,
                           false, &for_backups) != 0) {
      fd_backup_restore(&for_backups);
      redir_vec_free(&runtime_redirs);
      status = 1;
      goto done;
    }
    for_redir_applied = true;
  }

  if (node->data.for_cmd.implicit_words) {
    for_expanded.items =
        arena_xmalloc(sizeof(*for_expanded.items) * state->positional_count);
    for_expanded.len = state->positional_count;
    for (i = 0; i < state->positional_count; i++) {
      for_expanded.items[i] = arena_xstrdup(state->positional_params[i]);
    }
  } else if (node->data.for_cmd.words[0] != '\0') {
    if (lexer_split_words(node->data.for_cmd.words, &for_lexed) != 0) {
      status = 2;
      goto done;
    }
    if (simple_command_collect_words_and_redirs(&for_lexed, &for_raw_words,
                                                &for_redirs) != 0) {
      status = 2;
      goto done;
    }
    if (for_redirs.len != 0) {
      posish_errorf("for: redirection in word list is not supported");
      status = 2;
      goto done;
    }

    for_in.items = for_raw_words.items;
    for_in.len = for_raw_words.len;
    if (expand_words(&for_in, &for_expanded, state, true) != 0) {
      status = 2;
      goto done;
    }
  }

  state->loop_depth++;
  for (i = 0; i < for_expanded.len && !state->should_exit; i++) {
    if (vars_set_assignment(state, node->data.for_cmd.name,
                            for_expanded.items[i], true) != 0) {
      status = 1;
      if (!state->interactive) {
        state->should_exit = true;
        state->exit_status = status;
      }
      break;
    }

    status = execute_ast_node(state, node->data.for_cmd.body_node, true);
    maybe_trigger_errexit(state, status);
    if (state->should_exit || state->return_requested) {
      break;
    }
    if (state->break_levels > 0) {
      state->break_levels--;
      status = 0;
      break;
    }
    if (state->continue_levels > 0) {
      state->continue_levels--;
      status = 0;
      if (state->continue_levels > 0) {
        break;
      }
    }
  }
  state->loop_depth--;

done:
  if (for_redir_applied) {
    fd_backup_restore(&for_backups);
    redir_vec_free(&runtime_redirs);
  }
  lexer_free_tokens(&for_lexed);
  redir_vec_free(&for_redirs);
  simple_command_word_vec_free(&for_raw_words);
  lexer_free_tokens(&for_expanded);
  return status;
}

static int run_function_def_ast(struct shell_state *state,
                                const struct ast_node *node) {
  return functions_set(state, node->data.funcdef.name, node->data.funcdef.body,
                       &node->data.funcdef.redirs);
}

static int execute_case_ast_body_node(struct shell_state *state,
                                      const struct ast_node *node) {
  int status;

  status = execute_ast_node(state, node, true);
  maybe_trigger_errexit(state, status);
  return status;
}

static int run_case_ast(struct shell_state *state,
                        const struct ast_node *node) {
  struct fd_backup_vec backups;
  struct redir_vec runtime_redirs;
  bool redir_applied;
  int status;

  backups.items = NULL;
  backups.len = 0;
  runtime_redirs.items = NULL;
  runtime_redirs.len = 0;
  redir_applied = node->data.case_cmd.redirs.len != 0;

  if (redir_applied &&
      (prepare_runtime_redirections(state, &node->data.case_cmd.redirs,
                                    &runtime_redirs) != 0 ||
       apply_redirections(state, &runtime_redirs, true, state->noclobber, false,
                          &backups) != 0)) {
    fd_backup_restore(&backups);
    redir_vec_free(&runtime_redirs);
    return 1;
  }

  status = execute_structured_case_command(
      state, node->data.case_cmd.word_expr, node->data.case_cmd.clauses,
      node->data.case_cmd.clause_count, execute_program_text,
      execute_case_ast_body_node);

  if (redir_applied) {
    fd_backup_restore(&backups);
    redir_vec_free(&runtime_redirs);
  }
  return status;
}

static bool split_case_redirection_suffix(const char *source, char **core_out,
                                          char **suffix_out) {
  size_t i;
  int case_depth;
  int paren_depth;
  int brace_depth;
  char quote;
  size_t end_pos;

  *core_out = NULL;
  *suffix_out = NULL;

  i = 0;
  while (isspace((unsigned char)source[i])) {
    i++;
  }
  if (strncmp(source + i, "case", 4) != 0 || !keyword_boundary(source[i + 4])) {
    return false;
  }

  case_depth = 0;
  paren_depth = 0;
  brace_depth = 0;
  quote = '\0';
  end_pos = 0;

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
      if (paren_depth == 0 && brace_depth == 0 &&
          (isalpha((unsigned char)ch) || ch == '_') &&
          word_starts_command_position(source, i)) {
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
        boundary = skip_continuations_forward(source, j);
        if (keyword_boundary(source[boundary]) && source[boundary] != ')') {
          if (kwlen == 4 && strncmp(keyword, "case", 4) == 0) {
            case_depth++;
          } else if (kwlen == 4 && strncmp(keyword, "esac", 4) == 0 &&
                     case_depth > 0) {
            case_depth--;
            if (case_depth == 0) {
              end_pos = boundary;
              break;
            }
          }
        }
        i = j - 1;
        continue;
      }
      if (ch == '(') {
        paren_depth++;
        continue;
      }
      if (ch == ')' && paren_depth > 0) {
        paren_depth--;
        continue;
      }
      if (ch == '{') {
        brace_depth++;
        continue;
      }
      if (ch == '}' && brace_depth > 0) {
        brace_depth--;
        continue;
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

  if (case_depth != 0 || end_pos == 0) {
    return false;
  }

  *core_out = dup_trimmed_slice(source, 0, end_pos);
  *suffix_out = dup_trimmed_slice(source, end_pos, strlen(source));
  if ((*suffix_out)[0] == '\0') {
    arena_maybe_free(*core_out);
    arena_maybe_free(*suffix_out);
    *core_out = NULL;
    *suffix_out = NULL;
    return false;
  }

  return true;
}

static bool run_legacy_case_command(struct shell_state *state, char *trimmed,
                                    int *status_out) {
  char *case_core;
  char *case_redir_suffix;
  int status;

  case_core = NULL;
  case_redir_suffix = NULL;
  if (split_case_redirection_suffix(trimmed, &case_core, &case_redir_suffix)) {
    struct redir_vec case_redirs;
    struct fd_backup_vec case_backups;

    case_redirs.items = NULL;
    case_redirs.len = 0;
    case_backups.items = NULL;
    case_backups.len = 0;
    status = 2;

    if (simple_command_parse_redirections_from_source(case_redir_suffix, state,
                                                      &case_redirs) != 0) {
      goto case_done;
    }
    if (apply_redirections(state, &case_redirs, true, state->noclobber, false,
                           &case_backups) != 0) {
      fd_backup_restore(&case_backups);
      status = 1;
      goto case_done;
    }

    if (try_execute_case_command(state, case_core, &status,
                                 execute_program_text)) {
      fd_backup_restore(&case_backups);
    } else {
      fd_backup_restore(&case_backups);
      status = 2;
    }

  case_done:
    redir_vec_free(&case_redirs);
    arena_maybe_free(case_core);
    arena_maybe_free(case_redir_suffix);
    arena_maybe_free(trimmed);
    *status_out = status;
    return true;
  }

  if (try_execute_case_command(state, trimmed, &status, execute_program_text)) {
    arena_maybe_free(trimmed);
    *status_out = status;
    return true;
  }

  return false;
}

static int run_legacy_atom_fallback(struct shell_state *state, char *trimmed,
                                    bool allow_builtin) {
  char *fn_name;
  char *fn_body;
  char *inner;
  char *subshell_redirs;
  char *brace_inner;
  char *brace_redirs;
  int status;

  fn_name = NULL;
  fn_body = NULL;
  inner = NULL;
  subshell_redirs = NULL;
  brace_inner = NULL;
  brace_redirs = NULL;

  if (parse_function_definition(trimmed, &fn_name, &fn_body)) {
    status = functions_set(state, fn_name, fn_body, NULL);
    arena_maybe_free(fn_name);
    arena_maybe_free(fn_body);
    arena_maybe_free(trimmed);
    return status;
  }

  if (run_legacy_case_command(state, trimmed, &status)) {
    return status;
  }

  if (try_execute_alt_parameter_command(state, trimmed, &status)) {
    arena_maybe_free(trimmed);
    return status;
  }

  if (unwrap_subshell_group(trimmed, &inner, &subshell_redirs)) {
    status = run_group_with_redirection_source(state, inner, subshell_redirs,
                                               run_subshell_command);
    arena_maybe_free(inner);
    arena_maybe_free(subshell_redirs);
  } else if (unwrap_brace_group(trimmed, &brace_inner, &brace_redirs)) {
    status = run_group_with_redirection_source(state, brace_inner, brace_redirs,
                                               execute_program_text);
    arena_maybe_free(brace_inner);
    arena_maybe_free(brace_redirs);
  } else {
    status = simple_command_execute(state, trimmed, allow_builtin,
                                    execute_program_text);
  }

  arena_maybe_free(trimmed);
  return status;
}

static int execute_command_atom(struct shell_state *state, const char *source,
                                bool allow_builtin) {
  char *collapsed;
  char *trimmed;
  int status;

  trimmed = dup_slice(source, 0, strlen(source));
  collapsed = program_text_collapse_line_continuations(trimmed);
  arena_maybe_free(trimmed);
  trimmed = dup_trimmed_slice(collapsed, 0, strlen(collapsed));
  arena_maybe_free(collapsed);
  if (trimmed[0] == '\0') {
    arena_maybe_free(trimmed);
    return 0;
  }

  /*
   * `set +n` (or `set +o noexec`) is allowed to run while in noexec mode so
   * scripts can turn execution back on. Everything else is parse-only.
   */
  if (state->noexec && !exec_noexec_allows_set_toggle(trimmed)) {
    arena_maybe_free(trimmed);
    return 0;
  }

  if (try_run_ast_compound_command(state, trimmed, allow_builtin, &status)) {
    arena_maybe_free(trimmed);
    return status;
  }

  return run_legacy_atom_fallback(state, trimmed, allow_builtin);
}

static int run_command_child_body(struct shell_state *state,
                                  const void *payload) {
  const char *source;

  source = payload;
  return execute_command_atom(state, source, true);
}

static int run_program_text_child_body(struct shell_state *state,
                                       const void *payload) {
  const char *source;

  source = payload;
  return execute_program_text(state, source);
}

static int run_ast_child_body(struct shell_state *state, const void *payload) {
  const struct ast_node *node;

  node = payload;
  return execute_ast_node(state, node, true);
}

static void exec_child_command(struct shell_state *parent_state,
                               const char *source) {
  spawn_exec_child_payload(parent_state, run_command_child_body, source);
}

static const struct program_text_hooks *get_program_text_hooks(void) {
  static const struct program_text_hooks hooks = {
      .run_command_atom = execute_command_atom,
      .exec_child_command = exec_child_command,
      .run_async_list = run_async_list,
      .try_run_ast_compound_command = try_run_ast_compound_command,
      .has_pending_flow_control = has_pending_flow_control,
  };

  return &hooks;
}

static void exec_child_ast_node(struct shell_state *parent_state,
                                const struct ast_node *node) {
  spawn_exec_child_payload(parent_state, run_ast_child_body, node);
}

static int run_async_ast_node(struct shell_state *state,
                              const struct ast_node *node) {
  return spawn_run_async_payload(state, node, node->source, run_ast_child_body);
}

static int execute_ast_pipeline(struct shell_state *state,
                                const struct ast_node *node) {
  pid_t *pids;
  pid_t pipeline_pgid;
  bool isolate_pipeline_pgid;
  bool pipefail_snapshot;
  int *command_statuses;
  int *wait_statuses;
  bool *have_wait_statuses;
  int last_status;
  int in_fd;
  size_t i;

  if (node->data.pipeline.len == 0) {
    return 0;
  }
  if (node->data.pipeline.len == 1) {
    int status;
    bool ignored;

    status = execute_ast_node(state, node->data.pipeline.items[0], true);
    ignored = state->errexit_ignored;
    state->errexit_ignored = status != 0 && ignored;
    state->last_status = status;
    if (node->data.pipeline.negate) {
      state->errexit_ignored = true;
      state->last_status = status == 0 ? 1 : 0;
      return state->last_status;
    }
    return status;
  }

  pids = arena_xmalloc(sizeof(*pids) * node->data.pipeline.len);
  command_statuses =
      arena_xmalloc(sizeof(*command_statuses) * node->data.pipeline.len);
  wait_statuses =
      arena_xmalloc(sizeof(*wait_statuses) * node->data.pipeline.len);
  have_wait_statuses =
      arena_xmalloc(sizeof(*have_wait_statuses) * node->data.pipeline.len);
  memset(wait_statuses, 0, sizeof(*wait_statuses) * node->data.pipeline.len);
  memset(have_wait_statuses, 0,
         sizeof(*have_wait_statuses) * node->data.pipeline.len);
  pipeline_pgid = -1;
  isolate_pipeline_pgid = state->monitor_mode && state->main_context;
  pipefail_snapshot = state->pipefail;
  in_fd = -1;

  for (i = 0; i < node->data.pipeline.len; i++) {
    int pipefd[2];
    pid_t pid;

    pipefd[0] = -1;
    pipefd[1] = -1;

    if (i + 1 < node->data.pipeline.len) {
      if (pipe(pipefd) != 0) {
        perror("pipe");
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

      exec_child_ast_node(state, node->data.pipeline.items[i]);
    }

    pids[i] = pid;
    if (isolate_pipeline_pgid) {
      if (pipeline_pgid <= 0) {
        pipeline_pgid = pid;
      }
      if (setpgid(pid, pipeline_pgid) != 0 && errno != EACCES &&
          errno != ESRCH && errno != EPERM && errno != EINVAL) {
        /* keep running */
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
  for (i = 0; i < node->data.pipeline.len; i++) {
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
      status_pid = pids[node->data.pipeline.len - 1];
      jobs_track_job(job_pgid, pids, node->data.pipeline.len, status_pid,
                     node->source, true);
      for (j = 0; j < node->data.pipeline.len; j++) {
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
    command_statuses[i] = command_status;
  }

  if (pipefail_snapshot) {
    int last_non_zero;

    last_non_zero = 0;
    for (i = 0; i < node->data.pipeline.len; i++) {
      if (command_statuses[i] != 0) {
        last_non_zero = command_statuses[i];
      }
    }
    last_status = last_non_zero;
  } else {
    last_status = command_statuses[node->data.pipeline.len - 1];
  }

  if (node->data.pipeline.negate) {
    state->errexit_ignored = true;
    state->last_status = last_status == 0 ? 1 : 0;
    return state->last_status;
  }
  state->errexit_ignored = false;
  state->last_status = last_status;
  return last_status;
}

static int execute_ast_node(struct shell_state *state,
                            const struct ast_node *node, bool allow_builtin) {
  size_t i;
  int status;

  if (node == NULL) {
    return 0;
  }

  if (node->kind != AST_NODE_EMPTY && node->kind != AST_NODE_SEQUENCE &&
      node->kind != AST_NODE_AND_OR) {
    set_lineno_for_ast_node(state, node);
  }

  switch (node->kind) {
  case AST_NODE_EMPTY:
    status = 0;
    break;
  case AST_NODE_SEQUENCE:
    status = 0;
    for (i = 0; i < node->data.list.len; i++) {
      status = execute_ast_node(state, node->data.list.items[i], true);
      state->last_status = status;
      shell_run_pending_traps(state);
      maybe_trigger_errexit(state, status);
      if (state->should_exit || has_pending_flow_control(state)) {
        break;
      }
    }
    break;
  case AST_NODE_ASYNC:
    status = run_async_ast_node(state, node->data.unary.child);
    break;
  case AST_NODE_AND_OR:
    status = execute_ast_node(state, node->data.andor.items[0], true);
    if (state->should_exit || has_pending_flow_control(state)) {
      break;
    }
    for (i = 0; i + 1 < node->data.andor.len; i++) {
      if (node->data.andor.ops[i] == AST_ANDOR_AND) {
        if (status == 0) {
          status = execute_ast_node(state, node->data.andor.items[i + 1], true);
        } else {
          state->errexit_ignored = true;
        }
      } else if (status != 0) {
        status = execute_ast_node(state, node->data.andor.items[i + 1], true);
      }
      if (state->should_exit || has_pending_flow_control(state)) {
        break;
      }
    }
    break;
  case AST_NODE_PIPELINE:
    status = execute_ast_pipeline(state, node);
    break;
  case AST_NODE_SIMPLE_COMMAND:
    status = run_simple_ast(state, node, allow_builtin);
    break;
  case AST_NODE_SUBSHELL:
    status = ast_exec_run_subshell_group(state, node, run_subshell_command);
    break;
  case AST_NODE_BRACE_GROUP:
    status = ast_exec_run_brace_group(state, node, execute_program_text);
    break;
  case AST_NODE_IF:
    status = ast_exec_run_if(state, node, execute_ast_node,
                             maybe_trigger_errexit, has_pending_flow_control);
    break;
  case AST_NODE_WHILE:
    status = ast_exec_run_loop(state, node, false, execute_ast_node,
                               maybe_trigger_errexit);
    break;
  case AST_NODE_UNTIL:
    status = ast_exec_run_loop(state, node, true, execute_ast_node,
                               maybe_trigger_errexit);
    break;
  case AST_NODE_FOR:
    status = run_for_ast(state, node);
    break;
  case AST_NODE_FUNCTION_DEF:
    status = run_function_def_ast(state, node);
    break;
  case AST_NODE_CASE:
    status = run_case_ast(state, node);
    break;
  case AST_NODE_LEGACY:
    status = execute_program_text(state, node->source);
    break;
  default:
    status = 1;
    break;
  }

  return status;
}

static void maybe_trigger_errexit(struct shell_state *state, int status) {
  if (status != 0 && state->errexit && !state->interactive &&
      !state->errexit_ignored && !state->should_exit &&
      !has_pending_flow_control(state)) {
    state->should_exit = true;
    state->exit_status = status;
  }
}

static int run_simple_ast(struct shell_state *state,
                          const struct ast_node *node, bool allow_builtin) {
  char *rewritten;
  bool changed;
  bool saved_suppress_aliases;
  int status;

  if (state->suppress_ast_aliases) {
    return simple_command_execute_parts(state, &node->data.simple.raw_words,
                                        &node->data.simple.redirs,
                                        allow_builtin, execute_program_text);
  }

  rewritten = NULL;
  changed = false;
  if (alias_rewrite_snippet(state, node->source, &rewritten, &changed) != 0) {
    arena_maybe_free(rewritten);
    return 2;
  }
  if (!changed) {
    arena_maybe_free(rewritten);
    return simple_command_execute_parts(state, &node->data.simple.raw_words,
                                        &node->data.simple.redirs,
                                        allow_builtin, execute_program_text);
  }
  if (rewritten == NULL || rewritten[0] == '\0') {
    arena_maybe_free(rewritten);
    return state->last_status;
  }

  saved_suppress_aliases = state->suppress_ast_aliases;
  state->suppress_ast_aliases = true;
  status = execute_program_text_internal(state, rewritten, false);
  state->suppress_ast_aliases = saved_suppress_aliases;
  return status;
}



static int execute_program_text(struct shell_state *state, const char *source) {
  return program_text_execute(state, source, get_program_text_hooks());
}

static int execute_program_text_internal(struct shell_state *state,
                                         const char *source,
                                         bool apply_aliases) {
  return program_text_execute_internal(state, source, apply_aliases,
                                       get_program_text_hooks());
}

int exec_run_program(struct shell_state *state,
                     const struct ast_program *program) {
  if (program == NULL || program->root == NULL) {
    return 0;
  }

  if (!ast_node_is_ast_owned(program->root)) {
    return execute_program_text(state, program->source);
  }

  return execute_ast_node(state, program->root, true);
}
