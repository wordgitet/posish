/* SPDX-License-Identifier: 0BSD */

/* posish - simple command execution helpers */

#include "simple_command.h"

#include "arena.h"
#include "builtins/builtin.h"
#include "error.h"
#include "exec.h"
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

enum prepared_kind {
  PREPARED_KIND_NONE,
  PREPARED_KIND_ASSIGN_ONLY,
  PREPARED_KIND_BUILTIN,
  PREPARED_KIND_FUNCTION,
  PREPARED_KIND_EXTERNAL,
};

struct prepared_command {
  struct ast_word_vec raw_words;
  struct token_vec expanded;
  struct token_vec assign_expanded;
  struct token_vec cmd_expanded;
  struct ast_word_vec words;
  struct redir_vec redirs;
  char **argv;
  size_t argc;
  size_t assign_count;
  bool special_name;
  bool assignment_special;
  bool persist_builtin_redirs;
  bool redirs_only_command;
  bool saw_cmdsub;
  int last_cmdsub_status;
  bool pre_expand_redirs;
  bool builtin_available;
  struct shell_function *function_def;
  enum prepared_kind kind;
};

static bool is_exec_without_command(char *const argv[]);
static bool is_command_exec_without_command(char *const argv[]);
static bool is_assignment_word(const char *word);
static void trace_simple_words(struct shell_state *state, char *const words[],
                               size_t count);
static bool is_reserved_word_as_command(const char *word);
static size_t declaration_utility_prefix_len(
    const struct ast_word_vec *raw_words, size_t assign_count);
static int apply_persistent_assignments(struct shell_state *state,
                                        char *const words[], size_t count);
static int apply_temporary_assignments(struct shell_state *state,
                                       char *const words[], size_t count,
                                       struct env_restore_vec *restore);
static void positional_push(struct shell_state *state, char *const argv[],
                            size_t argc, struct positional_backup *backup);
static void positional_pop(struct shell_state *state,
                           const struct positional_backup *backup);
static void prepared_command_refresh_dispatch(struct shell_state *state,
                                              struct prepared_command *cmd,
                                              bool allow_builtin);
static bool simple_command_try_literal_fast_path(struct shell_state *state,
                                                 const struct prepared_command *cmd,
                                                 bool allow_builtin,
                                                 int *status_out);
static void prepared_command_scan_assignments(struct prepared_command *cmd);
static int prepared_command_expand_command_words(struct shell_state *state,
                                                 struct prepared_command *cmd,
                                                 bool allow_builtin);
static int prepared_command_expand_runtime_parts(
    struct shell_state *state, struct prepared_command *cmd,
    struct fd_backup_vec *pre_expand_backups, bool allow_builtin);
static bool simple_command_try_single_assignment(
    struct shell_state *state, const struct prepared_command *cmd,
    int *status_out);
static int simple_command_run_redirs_only_child(
    struct shell_state *state, struct prepared_command *cmd);
static bool simple_command_try_empty_command(struct shell_state *state,
                                             struct prepared_command *cmd,
                                             struct fd_backup_vec *fd_backups,
                                             int *status_out);
static bool simple_command_try_assign_only(struct shell_state *state,
                                           struct prepared_command *cmd,
                                           struct fd_backup_vec *fd_backups,
                                           int *status_out);
static bool simple_command_try_empty_argv(struct shell_state *state,
                                          struct prepared_command *cmd,
                                          struct fd_backup_vec *fd_backups,
                                          int *status_out);
static bool simple_command_try_direct_builtin_dispatch(
    struct shell_state *state, const struct prepared_command *cmd,
    bool allow_builtin, int *status_out);
static int simple_command_apply_command_assignments(
    struct shell_state *state, const struct prepared_command *cmd,
    struct env_restore_vec *temp_env, bool *have_temp_env);
static int simple_command_dispatch(struct shell_state *state,
                                   struct prepared_command *cmd,
                                   bool allow_builtin,
                                   simple_command_body_runner run_body,
                                   struct fd_backup_vec *fd_backups);

static void env_restore_vec_free(struct env_restore_vec *restore) {
  /*
   * Restore vectors are command-scratch and reclaimed by the surrounding
   * command arena mark.
   */
  restore->items = NULL;
  restore->len = 0;
}

void simple_command_word_vec_free(struct ast_word_vec *words) {
  /*
   * Word vectors are arena-backed and reclaimed by the surrounding
   * command/script arena reset.
   */
  words->items = NULL;
  words->len = 0;
}

static void prepared_command_init(struct prepared_command *cmd,
                                  const struct ast_word_vec *ast_raw_words,
                                  const struct redir_vec *ast_redirs) {
  cmd->raw_words.items = ast_raw_words->items;
  cmd->raw_words.len = ast_raw_words->len;
  cmd->expanded.items = NULL;
  cmd->expanded.len = 0;
  cmd->assign_expanded.items = NULL;
  cmd->assign_expanded.len = 0;
  cmd->cmd_expanded.items = NULL;
  cmd->cmd_expanded.len = 0;
  cmd->words.items = NULL;
  cmd->words.len = 0;
  cmd->redirs.items = NULL;
  cmd->redirs.len = 0;
  cmd->argv = NULL;
  cmd->argc = 0;
  cmd->assign_count = 0;
  cmd->special_name = false;
  cmd->assignment_special = false;
  cmd->persist_builtin_redirs = false;
  cmd->redirs_only_command = false;
  cmd->saw_cmdsub = false;
  cmd->last_cmdsub_status = 0;
  cmd->pre_expand_redirs = false;
  cmd->builtin_available = false;
  cmd->function_def = NULL;
  cmd->kind = PREPARED_KIND_NONE;

  redir_vec_clone(&cmd->redirs, ast_redirs);
  cmd->redirs_only_command = cmd->raw_words.len == 0;
}

static void prepared_command_free(struct prepared_command *cmd) {
  /*
   * Prepared-command storage is command-scratch and reclaimed by the
   * surrounding command arena mark.
   */
  cmd->argv = NULL;
  redir_vec_free(&cmd->redirs);
  simple_command_word_vec_free(&cmd->words);
  simple_command_word_vec_free(&cmd->raw_words);
  lexer_free_tokens(&cmd->expanded);
  lexer_free_tokens(&cmd->assign_expanded);
  lexer_free_tokens(&cmd->cmd_expanded);
}

static void prepared_command_note_cmdsub(struct prepared_command *cmd,
                                         const struct shell_state *state) {
  if (!state->cmdsub_performed) {
    return;
  }
  cmd->saw_cmdsub = true;
  cmd->last_cmdsub_status = state->last_cmdsub_status;
}

static void prepared_command_merge_words(struct prepared_command *cmd) {
  size_t i;

  if (cmd->assign_expanded.len == 0) {
    cmd->words.items = cmd->cmd_expanded.items;
    cmd->words.len = cmd->cmd_expanded.len;
    cmd->cmd_expanded.items = NULL;
    cmd->cmd_expanded.len = 0;
    return;
  }

  if (cmd->cmd_expanded.len == 0) {
    cmd->words.items = cmd->assign_expanded.items;
    cmd->words.len = cmd->assign_expanded.len;
    cmd->assign_expanded.items = NULL;
    cmd->assign_expanded.len = 0;
    return;
  }

  cmd->expanded.len = cmd->assign_expanded.len + cmd->cmd_expanded.len;
  cmd->expanded.items = arena_xmalloc(sizeof(*cmd->expanded.items) * cmd->expanded.len);
  for (i = 0; i < cmd->assign_expanded.len; i++) {
    cmd->expanded.items[i] = cmd->assign_expanded.items[i];
  }
  for (i = 0; i < cmd->cmd_expanded.len; i++) {
    cmd->expanded.items[cmd->assign_expanded.len + i] = cmd->cmd_expanded.items[i];
  }
  cmd->words.items = cmd->expanded.items;
  cmd->words.len = cmd->expanded.len;
  cmd->expanded.items = NULL;
  cmd->expanded.len = 0;

  cmd->assign_expanded.items = NULL;
  cmd->assign_expanded.len = 0;
  cmd->cmd_expanded.items = NULL;
  cmd->cmd_expanded.len = 0;
}

static void prepared_command_classify(struct shell_state *state,
                                      struct prepared_command *cmd,
                                      bool allow_builtin) {
  size_t i;

  cmd->assign_count = 0;
  while (cmd->assign_count < cmd->words.len &&
         is_assignment_word(cmd->words.items[cmd->assign_count])) {
    cmd->assign_count++;
  }

  if (cmd->assign_count == cmd->words.len) {
    cmd->kind = PREPARED_KIND_ASSIGN_ONLY;
    return;
  }

  cmd->argc = cmd->words.len - cmd->assign_count;
  cmd->argv = arena_xmalloc(sizeof(*cmd->argv) * (cmd->argc + 1));
  for (i = 0; i < cmd->argc; i++) {
    cmd->argv[i] = cmd->words.items[cmd->assign_count + i];
  }
  cmd->argv[cmd->argc] = NULL;

  prepared_command_refresh_dispatch(state, cmd, allow_builtin);
}

static void prepared_command_refresh_dispatch(struct shell_state *state,
                                              struct prepared_command *cmd,
                                              bool allow_builtin) {
  cmd->special_name = allow_builtin && builtin_is_special_name(cmd->argv[0]);
  cmd->function_def =
      allow_builtin && !cmd->special_name ? functions_get_mut(state, cmd->argv[0]) : NULL;
  cmd->assignment_special =
      cmd->special_name && strcmp(cmd->argv[0], "command") != 0;
  cmd->persist_builtin_redirs =
      (cmd->special_name && is_exec_without_command(cmd->argv)) ||
      (allow_builtin && is_command_exec_without_command(cmd->argv));
  cmd->builtin_available =
      allow_builtin && builtin_is_name(cmd->argv[0]) &&
      (!builtin_is_substitutive_name(cmd->argv[0]) ||
       path_resolves_command(state, cmd->argv[0], false));

  if (cmd->function_def != NULL) {
    cmd->kind = PREPARED_KIND_FUNCTION;
  } else if (cmd->builtin_available) {
    cmd->kind = PREPARED_KIND_BUILTIN;
  } else {
    cmd->kind = PREPARED_KIND_EXTERNAL;
  }
}

static bool simple_command_try_literal_fast_path(struct shell_state *state,
                                                 const struct prepared_command *cmd,
                                                 bool allow_builtin,
                                                 int *status_out) {
  char *trace_words[1];

  if (!allow_builtin || state->noexec || cmd->redirs.len != 0 ||
      cmd->raw_words.len != 1) {
    return false;
  }

  trace_words[0] = cmd->raw_words.items[0];
  if (strcmp(cmd->raw_words.items[0], ":") == 0 ||
      strcmp(cmd->raw_words.items[0], "true") == 0) {
    trace_simple_words(state, trace_words, 1);
    state->cmdsub_performed = false;
    state->last_cmdsub_status = 0;
    *status_out = 0;
    return true;
  }
  if (strcmp(cmd->raw_words.items[0], "false") == 0) {
    trace_simple_words(state, trace_words, 1);
    state->cmdsub_performed = false;
    state->last_cmdsub_status = 0;
    *status_out = 1;
    return true;
  }

  return false;
}

static void prepared_command_scan_assignments(struct prepared_command *cmd) {
  cmd->assign_count = 0;
  while (cmd->assign_count < cmd->raw_words.len &&
         is_assignment_word(cmd->raw_words.items[cmd->assign_count])) {
    cmd->assign_count++;
  }
}

static int prepared_command_expand_command_words(struct shell_state *state,
                                                 struct prepared_command *cmd,
                                                 bool allow_builtin) {
  struct token_vec in_vec;

  prepared_command_scan_assignments(cmd);

  in_vec.items = cmd->raw_words.items + cmd->assign_count;
  in_vec.len = cmd->raw_words.len - cmd->assign_count;
  if (in_vec.len > 0 && is_reserved_word_as_command(in_vec.items[0])) {
    posish_error_idf(POSERR_UNEXPECTED_TOKEN, in_vec.items[0]);
    if (!state->interactive) {
      state->should_exit = true;
      state->exit_status = 2;
    }
    return 2;
  }

  if (in_vec.len > 0) {
    size_t decl_prefix_len;
    size_t wi;

    decl_prefix_len =
        declaration_utility_prefix_len(&cmd->raw_words, cmd->assign_count);
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
        return 2;
      }
      prepared_command_note_cmdsub(cmd, state);
      if (one_out.len > 0) {
        cmd->cmd_expanded.items = arena_xrealloc(
            cmd->cmd_expanded.items,
            sizeof(*cmd->cmd_expanded.items) *
                (cmd->cmd_expanded.len + one_out.len));
        for (oi = 0; oi < one_out.len; oi++) {
          cmd->cmd_expanded.items[cmd->cmd_expanded.len++] = one_out.items[oi];
        }
      }
    }
  }

  cmd->special_name = allow_builtin && cmd->cmd_expanded.len > 0 &&
                     cmd->cmd_expanded.items[0][0] != '\0' &&
                     builtin_is_special_name(cmd->cmd_expanded.items[0]);
  cmd->assignment_special =
      cmd->special_name && strcmp(cmd->cmd_expanded.items[0], "command") != 0;
  return 0;
}

static int prepared_command_expand_runtime_parts(
    struct shell_state *state, struct prepared_command *cmd,
    struct fd_backup_vec *pre_expand_backups, bool allow_builtin) {
  struct token_vec in_vec;

  if (prepared_command_expand_command_words(state, cmd, allow_builtin) != 0) {
    return 2;
  }

  if (!cmd->redirs_only_command) {
    bool redir_saw_cmdsub;
    int redir_last_cmdsub_status;
    int status;

    redir_saw_cmdsub = false;
    redir_last_cmdsub_status = 0;
    status = redir_expand_operands(state, &cmd->redirs, &redir_saw_cmdsub,
                                   &redir_last_cmdsub_status);
    if (redir_saw_cmdsub) {
      cmd->saw_cmdsub = true;
      cmd->last_cmdsub_status = redir_last_cmdsub_status;
    }
    if (status != 0) {
      return status;
    }
  }

  if (cmd->assign_count > 0 && cmd->cmd_expanded.len > 0 &&
      !cmd->assignment_special) {
    if (apply_redirections(state, &cmd->redirs, true, state->noclobber, false,
                           pre_expand_backups) != 0) {
      return 1;
    }
    cmd->pre_expand_redirs = true;
  }

  in_vec.items = cmd->raw_words.items;
  in_vec.len = cmd->assign_count;
  if (in_vec.len > 0) {
    if (expand_words(&in_vec, &cmd->assign_expanded, state, false) != 0) {
      return 2;
    }
    prepared_command_note_cmdsub(cmd, state);
  }

  if (cmd->pre_expand_redirs) {
    fd_backup_restore(pre_expand_backups);
    cmd->pre_expand_redirs = false;
  }

  state->cmdsub_performed = cmd->saw_cmdsub;
  state->last_cmdsub_status = cmd->saw_cmdsub ? cmd->last_cmdsub_status : 0;
  return 0;
}

static bool simple_command_try_single_assignment(
    struct shell_state *state, const struct prepared_command *cmd,
    int *status_out) {
  char *assign_words[1];

  if (cmd->redirs.len != 0 || cmd->raw_words.len != 1 || cmd->assign_count != 1 ||
      cmd->cmd_expanded.len != 0 || cmd->assign_expanded.len != 1 ||
      !is_assignment_word(cmd->assign_expanded.items[0])) {
    return false;
  }

  assign_words[0] = cmd->assign_expanded.items[0];
  trace_simple_words(state, assign_words, 1);
  *status_out = apply_persistent_assignments(state, assign_words, 1);
  if (*status_out == 0 && state->cmdsub_performed) {
    *status_out = state->last_cmdsub_status;
  }
  return true;
}

static int simple_command_run_redirs_only_child(
    struct shell_state *state, struct prepared_command *cmd) {
  pid_t pid;
  int wstatus;

  pid = fork();
  if (pid < 0) {
    perror("fork");
    return 1;
  }
  if (pid == 0) {
    struct shell_state local_state;
    bool redir_saw_cmdsub;
    int redir_last_cmdsub_status;
    int status;

    local_state = *state;
    arena_init(&local_state.arena_perm, state->arena_perm.default_block_size);
    arena_init(&local_state.arena_script, state->arena_script.default_block_size);
    arena_init(&local_state.arena_cmd, state->arena_cmd.default_block_size);
    arena_set_current(&local_state.arena_perm);
    local_state.should_exit = false;
    local_state.exit_status = 0;
    local_state.running_signal_trap = false;
    local_state.running_exit_trap = false;
    local_state.main_context = false;
    redir_saw_cmdsub = false;
    redir_last_cmdsub_status = 0;

    status = redir_expand_operands(&local_state, &cmd->redirs, &redir_saw_cmdsub,
                                   &redir_last_cmdsub_status);
    if (status != 0) {
      _exit(status);
    }
    if (redir_saw_cmdsub) {
      local_state.cmdsub_performed = true;
      local_state.last_cmdsub_status = redir_last_cmdsub_status;
    }
    if (apply_redirections(&local_state, &cmd->redirs, false,
                           local_state.noclobber, false, NULL) != 0) {
      _exit(1);
    }
    status = local_state.cmdsub_performed ? local_state.last_cmdsub_status : 0;
    _exit(status);
  }

  for (;;) {
    if (waitpid(pid, &wstatus, 0) < 0) {
      if (errno == EINTR) {
        shell_run_pending_traps(state);
        continue;
      }
      perror("waitpid");
      return 1;
    }
    break;
  }

  if (WIFEXITED(wstatus)) {
    return WEXITSTATUS(wstatus);
  }
  if (WIFSIGNALED(wstatus)) {
    return shell_status_from_wait_status(wstatus);
  }
  return 1;
}

static bool simple_command_try_empty_command(struct shell_state *state,
                                             struct prepared_command *cmd,
                                             struct fd_backup_vec *fd_backups,
                                             int *status_out) {
  if (cmd->words.len != 0) {
    return false;
  }

  if (cmd->redirs_only_command) {
    *status_out = simple_command_run_redirs_only_child(state, cmd);
    return true;
  }

  if (apply_redirections(state, &cmd->redirs, true, state->noclobber, false,
                         fd_backups) != 0) {
    fd_backup_restore(fd_backups);
    *status_out = 1;
    return true;
  }
  fflush(NULL);
  fd_backup_restore(fd_backups);
  if (state->cmdsub_performed) {
    *status_out = state->last_cmdsub_status;
    return true;
  }
  *status_out = 0;
  return true;
}

static bool simple_command_try_assign_only(struct shell_state *state,
                                           struct prepared_command *cmd,
                                           struct fd_backup_vec *fd_backups,
                                           int *status_out) {
  if (cmd->kind != PREPARED_KIND_ASSIGN_ONLY) {
    return false;
  }

  *status_out =
      apply_persistent_assignments(state, cmd->words.items, cmd->assign_count);
  if (*status_out == 0 && state->cmdsub_performed) {
    *status_out = state->last_cmdsub_status;
  }
  if (*status_out == 0) {
    if (apply_redirections(state, &cmd->redirs, true, state->noclobber, false,
                           fd_backups) != 0) {
      *status_out = 1;
    }
  }
  fflush(NULL);
  fd_backup_restore(fd_backups);
  return true;
}

static bool simple_command_try_empty_argv(struct shell_state *state,
                                          struct prepared_command *cmd,
                                          struct fd_backup_vec *fd_backups,
                                          int *status_out) {
  if (cmd->argc != 1 || cmd->argv[0][0] != '\0') {
    return false;
  }

  *status_out =
      apply_persistent_assignments(state, cmd->words.items, cmd->assign_count);
  if (*status_out == 0) {
    if (apply_redirections(state, &cmd->redirs, true, state->noclobber, false,
                           fd_backups) != 0) {
      *status_out = 1;
    }
  }
  fflush(NULL);
  fd_backup_restore(fd_backups);
  return true;
}

static bool simple_command_try_direct_builtin_dispatch(
    struct shell_state *state, const struct prepared_command *cmd,
    bool allow_builtin, int *status_out) {
  bool handled;

  if (!allow_builtin || cmd->assign_count != 0 || cmd->redirs.len != 0 ||
      cmd->function_def != NULL || !cmd->builtin_available) {
    return false;
  }

  handled = false;
  *status_out = builtin_dispatch(state, cmd->argv, &handled);
  return handled;
}

static int simple_command_apply_command_assignments(
    struct shell_state *state, const struct prepared_command *cmd,
    struct env_restore_vec *temp_env, bool *have_temp_env) {
  if (cmd->assign_count == 0) {
    return 0;
  }

  if (cmd->assignment_special) {
    return apply_persistent_assignments(state, cmd->words.items, cmd->assign_count);
  }

  if (apply_temporary_assignments(state, cmd->words.items, cmd->assign_count,
                                  temp_env) != 0) {
    return 1;
  }
  *have_temp_env = true;
  return 0;
}

static int simple_command_dispatch(struct shell_state *state,
                                   struct prepared_command *cmd,
                                   bool allow_builtin,
                                   simple_command_body_runner run_body,
                                   struct fd_backup_vec *fd_backups) {
  if (allow_builtin) {
    bool run_in_shell;
    bool handled;

    run_in_shell = cmd->function_def != NULL || cmd->builtin_available;
    handled = false;
    if (cmd->persist_builtin_redirs) {
      if (apply_redirections(state, &cmd->redirs, false, state->noclobber, false,
                             NULL) != 0) {
        if (cmd->special_name && !state->interactive) {
          state->should_exit = true;
          state->exit_status = 1;
        }
        return 1;
      }

      if (cmd->function_def != NULL) {
        handled = true;
      } else {
        int status;

        status = builtin_dispatch(state, cmd->argv, &handled);
        if (!handled) {
          return spawn_run_external_argv(state, cmd->argv, &cmd->redirs);
        }
        return status;
      }
    } else if (run_in_shell) {
      if (apply_redirections(state, &cmd->redirs, true, state->noclobber, false,
                             fd_backups) != 0) {
        if (cmd->special_name && !state->interactive) {
          state->should_exit = true;
          state->exit_status = 1;
        }
        fd_backup_restore(fd_backups);
        return 1;
      }

      if (cmd->function_def != NULL) {
        struct fd_backup_vec function_backups;
        struct redir_vec function_redirs;
        struct positional_backup positional_backup;
        bool function_redirs_applied;
        int status;

        positional_push(state, cmd->argv, cmd->argc, &positional_backup);
        state->function_depth++;
        function_backups.items = NULL;
        function_backups.len = 0;
        function_redirs.items = NULL;
        function_redirs.len = 0;
        function_redirs_applied = false;
        if (cmd->function_def->redirs.len > 0) {
          if (prepare_runtime_redirections(state, &cmd->function_def->redirs,
                                           &function_redirs) != 0 ||
              apply_redirections(state, &function_redirs, true,
                                 state->noclobber, false,
                                 &function_backups) != 0) {
            status = 1;
          } else {
            function_redirs_applied = true;
            status = 0;
          }
        } else {
          status = 0;
        }
        if (status == 0) {
          const struct ast_program *cached_program;

          cached_program = functions_get_cached_program(state, cmd->function_def);
          if (cached_program != NULL) {
            status = exec_run_program(state, cached_program);
          } else {
            status = run_body(state, cmd->function_def->body);
          }
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
        fflush(NULL);
        fd_backup_restore(fd_backups);
        return status;
      }

      {
        int status;

        status = builtin_dispatch(state, cmd->argv, &handled);
        fflush(NULL);
        fd_backup_restore(fd_backups);
        if (!handled) {
          return spawn_run_external_argv(state, cmd->argv, &cmd->redirs);
        }
        return status;
      }
    } else {
      return spawn_run_external_argv(state, cmd->argv, &cmd->redirs);
    }
  }

  return spawn_run_external_argv(state, cmd->argv, &cmd->redirs);
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
    heap_free(params[i]);
  }
  heap_free(params);
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

  state->positional_params =
      heap_xmalloc(sizeof(*state->positional_params) * state->positional_count);
  for (i = 0; i < state->positional_count; i++) {
    state->positional_params[i] = heap_xstrdup(argv[i + 1]);
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
      return 1;
    }
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
  struct prepared_command cmd;
  struct env_restore_vec temp_env;
  struct fd_backup_vec fd_backups;
  struct fd_backup_vec pre_expand_backups;
  int status;
  bool have_temp_env;
  struct arena *saved_arena;
  struct arena_mark command_mark;
  bool command_marked;

  prepared_command_init(&cmd, ast_raw_words, ast_redirs);
  temp_env.items = NULL;
  temp_env.len = 0;
  fd_backups.items = NULL;
  fd_backups.len = 0;
  pre_expand_backups.items = NULL;
  pre_expand_backups.len = 0;
  status = 0;
  have_temp_env = false;
  saved_arena = arena_get_current();
  command_mark.block = NULL;
  command_mark.used = 0;
  command_marked = saved_arena == &state->arena_cmd;
  if (command_marked) {
    arena_mark_take(&state->arena_cmd, &command_mark);
  }

  if (simple_command_try_literal_fast_path(state, &cmd, allow_builtin,
                                           &status)) {
    goto done;
  }

  status = prepared_command_expand_runtime_parts(state, &cmd,
                                                 &pre_expand_backups,
                                                 allow_builtin);
  if (status != 0) {
    goto done;
  }

  if (simple_command_try_single_assignment(state, &cmd, &status)) {
    goto done;
  }

  prepared_command_merge_words(&cmd);

  if (simple_command_try_empty_command(state, &cmd, &fd_backups, &status)) {
    goto done;
  }

  trace_simple_words(state, cmd.words.items, cmd.words.len);
  prepared_command_classify(state, &cmd, allow_builtin);

  if (simple_command_try_assign_only(state, &cmd, &fd_backups, &status)) {
    goto done;
  }

  if (simple_command_try_empty_argv(state, &cmd, &fd_backups, &status)) {
    goto done;
  }

  if (simple_command_try_direct_builtin_dispatch(state, &cmd, allow_builtin,
                                                 &status)) {
    goto done;
  }

  status = simple_command_apply_command_assignments(state, &cmd, &temp_env,
                                                    &have_temp_env);
  if (status != 0) {
    goto done;
  }

  if (cmd.kind != PREPARED_KIND_ASSIGN_ONLY && cmd.argv != NULL) {
    prepared_command_refresh_dispatch(state, &cmd, allow_builtin);
  }

  status = simple_command_dispatch(state, &cmd, allow_builtin, run_body,
                                   &fd_backups);

done:
  if (cmd.pre_expand_redirs) {
    fd_backup_restore(&pre_expand_backups);
  }
  if (have_temp_env) {
    restore_temporary_assignments(state, &temp_env);
    env_restore_vec_free(&temp_env);
  }

  if (status == 0 && state->cmdsub_performed && cmd.argc == 1 &&
      cmd.argv != NULL && cmd.argv[0][0] == '\0') {
    status = state->last_cmdsub_status;
  }

  prepared_command_free(&cmd);
  if (command_marked) {
    arena_mark_rewind(&state->arena_cmd, &command_mark);
  }
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
