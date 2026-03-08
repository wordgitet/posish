/* SPDX-License-Identifier: 0BSD */

/* posish - external command spawn helpers */

#include "spawn.h"

#include "arena.h"
#include "jobs.h"
#include "path.h"
#include "signals.h"
#include "trace.h"
#include "vars.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void exit_shell_child_status(int status) {
  int signo;

  if (shell_status_should_relay_signal(status, &signo)) {
    signal(signo, SIG_DFL);
    raise(signo);
  }
  _exit(status);
}

/*
 * Determine whether we can use vfork() instead of fork() for an
 * external command. vfork() avoids copying page tables and is
 * significantly faster, but the child shares the parent's address
 * space until exec, so we can only use it when:
 *
 * - There are no redirections (dup2/close would affect the parent)
 * - The shell is not interactive (complex tty/signal state)
 * - Job control is off (setpgid in child would race with parent)
 * - No traps are set (trap handlers reference parent memory)
 * - The user has not opted out via POSISH_NO_VFORK
 */
static bool can_use_vfork(const struct shell_state *state,
                          const struct redir_vec *redirs) {
  int signo;

  if (redirs != NULL && redirs->len > 0) {
    return false;
  }
  if (state->interactive) {
    return false;
  }
  if (state->monitor_mode) {
    return false;
  }
  for (signo = 1; signo < NSIG; signo++) {
    if (state->signal_traps[signo] != NULL) {
      return false;
    }
  }
  if (getenv("POSISH_NO_VFORK") != NULL) {
    return false;
  }
  return true;
}

static bool path_supports_direct_execve(const char *path) {
  unsigned char hdr[4];
  int fd;
  ssize_t nread;

  fd = open(path, O_RDONLY);
  if (fd < 0) {
    return false;
  }

  nread = read(fd, hdr, sizeof(hdr));
  close(fd);
  if (nread < 2) {
    return false;
  }

  if (hdr[0] == '#' && hdr[1] == '!') {
    return true;
  }

  if (nread >= 4 && hdr[0] == 0x7f && hdr[1] == 'E' && hdr[2] == 'L' &&
      hdr[3] == 'F') {
    return true;
  }

  return false;
}

static void write_stderr_best_effort(const char *text, size_t len) {
  while (len > 0) {
    ssize_t written;

    written = write(STDERR_FILENO, text, len);
    if (written <= 0) {
      break;
    }
    text += (size_t)written;
    len -= (size_t)written;
  }
}

static int run_external_argv_vfork(struct shell_state *state, const char *path,
                                   char *const argv[]) {
  pid_t pid;
  int status;
  char **envp;

  trace_log(POSISH_TRACE_SIGNALS, "vfork external argv0=%s", argv[0]);

  /*
   * Build the environment array BEFORE vfork.
   * vars_build_exec_envp allocates heap memory which is not
   * safe inside a vfork child.
   */
  envp = vars_build_exec_envp(state);

  /*
   * Flush all stdio buffers before vfork. The child shares the
   * parent's address space, so unflushed data would be written
   * twice after execve fails or after _exit.
   */
  fflush(NULL);

  pid = vfork();
  if (pid < 0) {
    perror("vfork");
    vars_free_envp(state, envp);
    return 1;
  }

  if (pid == 0) {
    /*
     * vfork child: we share the parent's memory.
     * Only async-signal-safe functions are legal here.
     * No malloc, no free, no arena ops, no stdio.
     *
     * exec_prepare_signals_for_exec_child only calls sigaction()
     * which is async-signal-safe and does not modify heap state.
     */
    exec_prepare_signals_for_exec_child(state);
    execve(path, argv, envp);
    {
      const char *msg = ": exec failed\n";

      write_stderr_best_effort(argv[0], strlen(argv[0]));
      write_stderr_best_effort(msg, strlen(msg));
    }
    _exit(errno == ENOENT ? 127 : 126);
  }

  vars_free_envp(state, envp);
  trace_log(POSISH_TRACE_SIGNALS, "waiting vfork external pid=%ld", (long)pid);

  for (;;) {
    if (waitpid(pid, &status, 0) < 0) {
      if (errno == EINTR) {
        continue;
      }
      perror("waitpid");
      return 1;
    }
    break;
  }

  if (WIFEXITED(status)) {
    trace_log(POSISH_TRACE_SIGNALS, "vfork external pid=%ld exited=%d",
              (long)pid, WEXITSTATUS(status));
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    trace_log(POSISH_TRACE_SIGNALS, "vfork external pid=%ld signaled sig=%d",
              (long)pid, WTERMSIG(status));
    return shell_status_from_wait_status(status);
  }
  return 1;
}

int spawn_run_external_argv(struct shell_state *state, char *const argv[],
                            const struct redir_vec *redirs) {
  char *path;
  int status;
  pid_t pid;

  trace_log(POSISH_TRACE_SIGNALS, "spawn external argv0=%s", argv[0]);
  path = path_resolve_command(state, argv[0], false);
  if (path == NULL) {
    int saved_errno;

    if (errno == 0) {
      errno = ENOENT;
    }
    saved_errno = errno;
    perror(argv[0]);
    return saved_errno == ENOENT ? 127 : 126;
  }

  if (can_use_vfork(state, redirs) && path_supports_direct_execve(path)) {
    status = run_external_argv_vfork(state, path, argv);
    arena_maybe_free(path);
    return status;
  }

  pid = fork();
  if (pid < 0) {
    perror("fork");
    arena_maybe_free(path);
    return 1;
  }

  if (pid == 0) {
    if (state->monitor_mode) {
      (void)setpgid(0, 0);
    }

    exec_prepare_signals_for_exec_child(state);
    if (apply_redirections(state, redirs, false, state->noclobber, false,
                           NULL) != 0) {
      _exit(1);
    }
    status = exec_replace_with_utility(state, path, argv);
    perror(argv[0]);
    _exit(status);
  }
  arena_maybe_free(path);

  if (state->monitor_mode) {
    (void)setpgid(pid, pid);
  }

  trace_log(POSISH_TRACE_SIGNALS, "waiting external pid=%ld", (long)pid);

  for (;;) {
    if (waitpid(pid, &status, WUNTRACED) < 0) {
      if (errno == EINTR) {
        shell_run_pending_traps(state);
        continue;
      }
      perror("waitpid");
      return 1;
    }
    break;
  }

  if (WIFEXITED(status)) {
    trace_log(POSISH_TRACE_SIGNALS, "external pid=%ld exited=%d", (long)pid,
              WEXITSTATUS(status));
    return WEXITSTATUS(status);
  }
  if (WIFSTOPPED(status)) {
    jobs_track_job(pid, &pid, 1, pid, argv[0], true);
    jobs_note_process_status(pid, status);
    trace_log(POSISH_TRACE_SIGNALS, "external pid=%ld stopped sig=%d",
              (long)pid, WSTOPSIG(status));
    return shell_status_from_wait_status(status);
  }
  if (WIFSIGNALED(status)) {
    trace_log(POSISH_TRACE_SIGNALS, "external pid=%ld signaled sig=%d",
              (long)pid, WTERMSIG(status));
    return shell_status_from_wait_status(status);
  }
  return 1;
}

int exec_replace_with_utility(struct shell_state *state, const char *path,
                              char *const argv[]) {
  char **envp;
  int saved_errno;

  envp = vars_build_exec_envp(state);
  execve(path, argv, envp);
  saved_errno = errno;

  if (saved_errno == ENOEXEC) {
    char *sh_path;
    char **sh_argv;
    size_t argc;
    size_t i;

    argc = 0;
    while (argv[argc] != NULL) {
      argc++;
    }

    sh_argv = arena_alloc_in(NULL, sizeof(*sh_argv) * (argc + 2));
    sh_argv[0] = (char *)"sh";
    sh_argv[1] = (char *)path;
    for (i = 1; i < argc; i++) {
      sh_argv[i + 1] = argv[i];
    }
    sh_argv[argc + 1] = NULL;

    sh_path = path_resolve_command(state, "sh", true);
    if (sh_path == NULL) {
      sh_path = arena_xstrdup("/bin/sh");
    }

    execve(sh_path, sh_argv, envp);
    saved_errno = errno;
    arena_maybe_free(sh_path);
    arena_maybe_free(sh_argv);
  }

  vars_free_envp(state, envp);
  errno = saved_errno;
  return saved_errno == ENOENT ? 127 : 126;
}

void spawn_exec_child_payload(struct shell_state *parent_state,
                              spawn_body_runner run_body,
                              const void *payload) {
  struct shell_state local_state;
  struct arena_mark child_mark;
  int status;

  local_state = *parent_state;
  arena_init(&local_state.arena_perm,
             parent_state->arena_perm.default_block_size);
  arena_init(&local_state.arena_script,
             parent_state->arena_script.default_block_size);
  arena_init(&local_state.arena_cmd,
             parent_state->arena_cmd.default_block_size);
  arena_set_current(&local_state.arena_cmd);
  arena_mark_take(&local_state.arena_cmd, &child_mark);
  local_state.should_exit = false;
  local_state.exit_status = 0;
  local_state.running_signal_trap = false;
  local_state.running_exit_trap = false;
  local_state.main_context = false;

  status = run_body(&local_state, payload);
  arena_mark_rewind(&local_state.arena_cmd, &child_mark);
  if (local_state.should_exit) {
    status = local_state.exit_status;
  }
  fflush(NULL);
  _exit(status);
}

int spawn_run_subshell_payload(struct shell_state *parent_state,
                               const void *payload, const char *job_source,
                               spawn_body_runner run_body) {
  pid_t pid;
  int status;

  trace_log(POSISH_TRACE_SIGNALS, "spawn subshell");
  pid = fork();
  if (pid < 0) {
    perror("fork");
    return 1;
  }

  if (pid == 0) {
    struct shell_state local_state;
    int st;

    if (parent_state->monitor_mode) {
      (void)setpgid(0, 0);
    }

    local_state = *parent_state;
    arena_init(&local_state.arena_perm,
               parent_state->arena_perm.default_block_size);
    arena_init(&local_state.arena_script,
               parent_state->arena_script.default_block_size);
    arena_init(&local_state.arena_cmd,
               parent_state->arena_cmd.default_block_size);
    arena_set_current(&local_state.arena_perm);
    local_state.should_exit = false;
    local_state.exit_status = 0;
    local_state.running_signal_trap = false;
    local_state.running_exit_trap = false;
    local_state.main_context = false;
    signals_reset_traps_for_child(&local_state);
    signals_reset_exit_trap_for_child(&local_state);

    st = run_body(&local_state, payload);
    shell_run_pending_traps(&local_state);
    shell_run_exit_trap(&local_state);
    if (local_state.should_exit) {
      st = local_state.exit_status;
    }
    fflush(NULL);
    exit_shell_child_status(st);
  }

  if (parent_state->monitor_mode) {
    (void)setpgid(pid, pid);
  }

  for (;;) {
    if (waitpid(pid, &status, WUNTRACED) < 0) {
      if (errno == EINTR) {
        continue;
      }
      perror("waitpid");
      return 1;
    }
    break;
  }

  if (WIFEXITED(status)) {
    trace_log(POSISH_TRACE_SIGNALS, "subshell pid=%ld exited=%d", (long)pid,
              WEXITSTATUS(status));
    return WEXITSTATUS(status);
  }
  if (WIFSTOPPED(status)) {
    jobs_track_job(pid, &pid, 1, pid, job_source, true);
    jobs_note_process_status(pid, status);
    trace_log(POSISH_TRACE_SIGNALS, "subshell pid=%ld stopped sig=%d",
              (long)pid, WSTOPSIG(status));
    return shell_status_from_wait_status(status);
  }
  if (WIFSIGNALED(status)) {
    trace_log(POSISH_TRACE_SIGNALS, "subshell pid=%ld signaled sig=%d",
              (long)pid, WTERMSIG(status));
    return shell_status_from_wait_status(status);
  }
  return 1;
}

int spawn_run_async_payload(struct shell_state *state, const void *payload,
                            const char *job_source,
                            spawn_body_runner run_body) {
  pid_t pid;

  trace_log(POSISH_TRACE_SIGNALS, "spawn async payload source=%s", job_source);
  pid = fork();
  if (pid < 0) {
    perror("fork");
    return 1;
  }

  if (pid == 0) {
    struct shell_state local_state;
    int st;
    int nullfd;

    if (state->monitor_mode) {
      (void)setpgid(0, 0);
    }

    local_state = *state;
    arena_init(&local_state.arena_perm, state->arena_perm.default_block_size);
    arena_init(&local_state.arena_script,
               state->arena_script.default_block_size);
    arena_init(&local_state.arena_cmd, state->arena_cmd.default_block_size);
    arena_set_current(&local_state.arena_perm);
    local_state.should_exit = false;
    local_state.exit_status = 0;
    local_state.running_signal_trap = false;
    local_state.running_exit_trap = false;
    local_state.in_async_context = true;
    local_state.main_context = false;
    signals_reset_traps_for_child(&local_state);
    signals_reset_exit_trap_for_child(&local_state);
#ifdef SIGINT
    if (!state->monitor_mode) {
      (void)signals_set_ignored(SIGINT);
      local_state.signal_cleared[SIGINT] = false;
    }
#endif
#ifdef SIGQUIT
    if (!state->monitor_mode) {
      (void)signals_set_ignored(SIGQUIT);
      local_state.signal_cleared[SIGQUIT] = false;
    }
#endif
    if (!state->monitor_mode) {
      nullfd = open("/dev/null", O_RDONLY);
      if (nullfd >= 0) {
        if (dup2(nullfd, STDIN_FILENO) < 0) {
          perror("dup2");
          _exit(1);
        }
        if (nullfd != STDIN_FILENO) {
          close(nullfd);
        }
      }
    }

    st = run_body(&local_state, payload);
    shell_run_pending_traps(&local_state);
    shell_run_exit_trap(&local_state);
    if (local_state.should_exit) {
      st = local_state.exit_status;
    }
    fflush(NULL);
    exit_shell_child_status(st);
  }

  if (state->monitor_mode) {
    (void)setpgid(pid, pid);
  }

  state->last_async_pid = pid;
  jobs_track_async(pid, pid, job_source);
  trace_log(POSISH_TRACE_SIGNALS, "async payload pid=%ld", (long)pid);
  return 0;
}
