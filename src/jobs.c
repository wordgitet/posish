/* SPDX-License-Identifier: 0BSD */

/* posish - job handling */

#include "jobs.h"
#include "arena.h"
#include "trace.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

struct tracked_process {
  pid_t pid;
  bool stopped;
  bool done;
  int wait_status;
  bool have_wait_status;
};

struct tracked_job {
  pid_t pgid;
  unsigned int job_id;
  unsigned long serial;
  bool stopped;
  char *command;
  pid_t status_pid;
  struct tracked_process *procs;
  size_t proc_count;
};

static struct tracked_job *g_jobs;
static size_t g_job_count;
static unsigned int g_next_job_id;
static unsigned long g_next_serial;

static char *dup_trimmed_command(const char *command) {
  size_t start;
  size_t end;
  char *copy;

  if (command == NULL) {
    return NULL;
  }

  start = 0;
  end = strlen(command);
  while (start < end && (command[start] == ' ' || command[start] == '\t' ||
                         command[start] == '\n')) {
    start++;
  }
  while (end > start && (command[end - 1] == ' ' || command[end - 1] == '\t' ||
                         command[end - 1] == '\n')) {
    end--;
  }

  copy = arena_alloc_in(NULL, end - start + 1);
  if (copy == NULL) {
    return NULL;
  }
  memcpy(copy, command + start, end - start);
  copy[end - start] = '\0';
  return copy;
}

static void free_job_storage(struct tracked_job *job) {
  if (job == NULL) {
    return;
  }
  arena_maybe_free(job->command);
  arena_maybe_free(job->procs);
  job->command = NULL;
  job->procs = NULL;
  job->proc_count = 0;
}

static void touch_job(struct tracked_job *job) {
  if (job != NULL) {
    job->serial = g_next_serial++;
  }
}

static struct tracked_job *find_job_by_pgid(pid_t pgid) {
  size_t i;

  for (i = 0; i < g_job_count; i++) {
    if (g_jobs[i].pgid == pgid) {
      return &g_jobs[i];
    }
  }
  return NULL;
}

static struct tracked_job *find_job_by_pid(pid_t pid) {
  size_t i;
  size_t j;

  for (i = 0; i < g_job_count; i++) {
    for (j = 0; j < g_jobs[i].proc_count; j++) {
      if (g_jobs[i].procs[j].pid == pid) {
        return &g_jobs[i];
      }
    }
  }
  return NULL;
}

static struct tracked_process *find_process(struct tracked_job *job, pid_t pid) {
  size_t i;

  if (job == NULL) {
    return NULL;
  }
  for (i = 0; i < job->proc_count; i++) {
    if (job->procs[i].pid == pid) {
      return &job->procs[i];
    }
  }
  return NULL;
}

static bool job_all_done(const struct tracked_job *job) {
  size_t i;

  if (job == NULL || job->proc_count == 0) {
    return false;
  }
  for (i = 0; i < job->proc_count; i++) {
    if (!job->procs[i].done) {
      return false;
    }
  }
  return true;
}

static bool job_any_stopped(const struct tracked_job *job) {
  size_t i;

  if (job == NULL) {
    return false;
  }
  for (i = 0; i < job->proc_count; i++) {
    if (job->procs[i].stopped && !job->procs[i].done) {
      return true;
    }
  }
  return false;
}

static void set_job_command(struct tracked_job *job, const char *command) {
  char *copy;

  if (job == NULL || command == NULL) {
    return;
  }

  copy = dup_trimmed_command(command);
  if (copy == NULL) {
    return;
  }

  arena_maybe_free(job->command);
  job->command = copy;
}

static bool fill_info_from_job(const struct tracked_job *job,
                               struct jobs_entry_info *out) {
  if (job == NULL || out == NULL) {
    return false;
  }

  out->pgid = job->pgid;
  out->status_pid = job->status_pid;
  out->job_id = job->job_id;
  out->command = job->command;
  out->stopped = job->stopped;
  return true;
}

static bool get_best_job(bool stopped_only, size_t rank,
                         struct jobs_entry_info *out) {
  size_t i;
  struct tracked_job *best;
  struct tracked_job *previous;

  best = NULL;
  previous = NULL;
  for (i = 0; i < g_job_count; i++) {
    struct tracked_job *job;

    job = &g_jobs[i];
    if (stopped_only && !job->stopped) {
      continue;
    }

    if (best == NULL || job->serial > best->serial) {
      previous = best;
      best = job;
      continue;
    }
    if (previous == NULL || job->serial > previous->serial) {
      previous = job;
    }
  }

  if (rank == 0) {
    return fill_info_from_job(best, out);
  }
  if (rank == 1) {
    return fill_info_from_job(previous, out);
  }
  return false;
}

static struct tracked_job *add_job(pid_t pgid, const pid_t *pids, size_t count,
                                   pid_t status_pid, const char *command,
                                   bool stopped) {
  struct tracked_job *new_jobs;
  struct tracked_job *job;
  size_t i;

  if (pgid <= 0 || pids == NULL || count == 0) {
    return NULL;
  }

  new_jobs = arena_realloc_in(NULL, g_jobs, sizeof(*g_jobs) * (g_job_count + 1));
  if (new_jobs == NULL) {
    return NULL;
  }
  g_jobs = new_jobs;

  job = &g_jobs[g_job_count];
  memset(job, 0, sizeof(*job));
  job->procs = arena_alloc_in(NULL, sizeof(*job->procs) * count);
  if (job->procs == NULL) {
    return NULL;
  }
  for (i = 0; i < count; i++) {
    job->procs[i].pid = pids[i];
    job->procs[i].stopped = false;
    job->procs[i].done = false;
    job->procs[i].wait_status = 0;
    job->procs[i].have_wait_status = false;
  }

  job->pgid = pgid;
  job->job_id = g_next_job_id++;
  job->serial = g_next_serial++;
  job->stopped = stopped;
  job->command = dup_trimmed_command(command);
  job->status_pid = status_pid > 0 ? status_pid : pids[count - 1];
  job->proc_count = count;
  g_job_count++;
  return job;
}

static void rebuild_job_processes(struct tracked_job *job, const pid_t *pids,
                                  size_t count) {
  struct tracked_process *new_procs;
  size_t i;

  if (job == NULL || pids == NULL || count == 0) {
    return;
  }

  new_procs = arena_alloc_in(NULL, sizeof(*new_procs) * count);
  if (new_procs == NULL) {
    return;
  }

  for (i = 0; i < count; i++) {
    struct tracked_process *old_proc;

    memset(&new_procs[i], 0, sizeof(new_procs[i]));
    new_procs[i].pid = pids[i];
    old_proc = find_process(job, pids[i]);
    if (old_proc != NULL) {
      new_procs[i] = *old_proc;
    }
  }

  arena_maybe_free(job->procs);
  job->procs = new_procs;
  job->proc_count = count;
}

static bool spec_all_digits(const char *text) {
  size_t i;

  if (text == NULL || text[0] == '\0') {
    return false;
  }
  for (i = 0; text[i] != '\0'; i++) {
    if (!isdigit((unsigned char)text[i])) {
      return false;
    }
  }
  return true;
}

void jobs_init(void) {
  jobs_destroy();
  g_next_job_id = 1;
  g_next_serial = 1;
  trace_log(POSISH_TRACE_JOBS, "jobs init");
}

void jobs_destroy(void) {
  size_t i;

  for (i = 0; i < g_job_count; i++) {
    free_job_storage(&g_jobs[i]);
  }
  arena_maybe_free(g_jobs);
  g_jobs = NULL;
  g_job_count = 0;
  g_next_job_id = 1;
  g_next_serial = 1;
}

void jobs_track_async(pid_t pid, pid_t pgid, const char *command) {
  jobs_track_job(pgid > 0 ? pgid : pid, &pid, 1, pid, command, false);
}

void jobs_track_job(pid_t pgid, const pid_t *pids, size_t count, pid_t status_pid,
                    const char *command, bool stopped) {
  struct tracked_job *job;

  if (pgid <= 0 || pids == NULL || count == 0) {
    return;
  }

  job = find_job_by_pgid(pgid);
  if (job == NULL) {
    job = add_job(pgid, pids, count, status_pid, command, stopped);
    if (job == NULL) {
      return;
    }
  } else {
    rebuild_job_processes(job, pids, count);
    job->pgid = pgid;
    job->status_pid = status_pid > 0 ? status_pid : pids[count - 1];
    job->stopped = stopped;
    set_job_command(job, command);
    touch_job(job);
  }

  trace_log(POSISH_TRACE_JOBS,
            "track job pgid=%ld status_pid=%ld count=%lu stopped=%d command=%s",
            (long)job->pgid, (long)job->status_pid, (unsigned long)job->proc_count,
            job->stopped ? 1 : 0, job->command == NULL ? "<none>" : job->command);
}

void jobs_note_process_status(pid_t pid, int wait_status) {
  struct tracked_job *job;
  struct tracked_process *proc;
  bool was_stopped;

  if (pid <= 0) {
    return;
  }

  job = find_job_by_pid(pid);
  if (job == NULL) {
    return;
  }

  proc = find_process(job, pid);
  if (proc == NULL) {
    return;
  }

  was_stopped = job->stopped;
  proc->have_wait_status = true;
  proc->wait_status = wait_status;
  if (WIFSTOPPED(wait_status)) {
    proc->stopped = true;
    proc->done = false;
  } else if (WIFEXITED(wait_status) || WIFSIGNALED(wait_status)) {
    proc->stopped = false;
    proc->done = true;
  } else {
    proc->stopped = false;
  }
  job->stopped = job_any_stopped(job);
  if (job->stopped && !was_stopped) {
    touch_job(job);
  }

  trace_log(POSISH_TRACE_JOBS,
            "note status pid=%ld pgid=%ld stopped=%d done=%d wait_status=%d",
            (long)pid, (long)job->pgid, proc->stopped ? 1 : 0, proc->done ? 1 : 0,
            wait_status);
}

void jobs_mark_job_running(pid_t pgid) {
  struct tracked_job *job;
  size_t i;

  job = find_job_by_pgid(pgid);
  if (job == NULL) {
    return;
  }

  job->stopped = false;
  for (i = 0; i < job->proc_count; i++) {
    job->procs[i].stopped = false;
  }
  touch_job(job);
  trace_log(POSISH_TRACE_JOBS, "mark running pgid=%ld", (long)pgid);
}

void jobs_forget_pgid(pid_t pgid) {
  size_t i;

  if (pgid <= 0) {
    return;
  }

  for (i = 0; i < g_job_count; i++) {
    if (g_jobs[i].pgid == pgid) {
      free_job_storage(&g_jobs[i]);
      g_jobs[i] = g_jobs[g_job_count - 1];
      g_job_count--;
      if (g_job_count == 0) {
        arena_maybe_free(g_jobs);
        g_jobs = NULL;
      } else {
        struct tracked_job *shrunk;

        shrunk = arena_realloc_in(NULL, g_jobs, sizeof(*g_jobs) * g_job_count);
        if (shrunk != NULL) {
          g_jobs = shrunk;
        }
      }
      trace_log(POSISH_TRACE_JOBS, "forget pgid=%ld", (long)pgid);
      return;
    }
  }
}

bool jobs_get_current(bool stopped_only, struct jobs_entry_info *out) {
  bool ok;

  ok = get_best_job(stopped_only, 0, out);
  if (ok) {
    trace_log(POSISH_TRACE_JOBS,
              "current stopped_only=%d pgid=%ld status_pid=%ld id=%u stopped=%d",
              stopped_only ? 1 : 0, (long)out->pgid, (long)out->status_pid,
              out->job_id, out->stopped ? 1 : 0);
  } else {
    trace_log(POSISH_TRACE_JOBS, "current stopped_only=%d <none>",
              stopped_only ? 1 : 0);
  }
  return ok;
}

bool jobs_get_previous(bool stopped_only, struct jobs_entry_info *out) {
  bool ok;

  ok = get_best_job(stopped_only, 1, out);
  if (ok) {
    trace_log(POSISH_TRACE_JOBS,
              "previous stopped_only=%d pgid=%ld status_pid=%ld id=%u stopped=%d",
              stopped_only ? 1 : 0, (long)out->pgid, (long)out->status_pid,
              out->job_id, out->stopped ? 1 : 0);
  } else {
    trace_log(POSISH_TRACE_JOBS, "previous stopped_only=%d <none>",
              stopped_only ? 1 : 0);
  }
  return ok;
}

enum jobs_lookup_result jobs_get_by_spec(const char *spec,
                                         struct jobs_entry_info *out) {
  size_t i;
  struct tracked_job *match;

  if (spec == NULL || spec[0] != '%') {
    return JOBS_LOOKUP_INVALID;
  }

  if (strcmp(spec, "%") == 0 || strcmp(spec, "%+") == 0 ||
      strcmp(spec, "%%") == 0) {
    return jobs_get_current(false, out) ? JOBS_LOOKUP_OK : JOBS_LOOKUP_NO_MATCH;
  }
  if (strcmp(spec, "%-") == 0) {
    return jobs_get_previous(false, out) ? JOBS_LOOKUP_OK : JOBS_LOOKUP_NO_MATCH;
  }

  if (spec[1] == '?') {
    const char *pattern;

    pattern = spec + 2;
    if (pattern[0] == '\0') {
      return JOBS_LOOKUP_INVALID;
    }

    match = NULL;
    for (i = 0; i < g_job_count; i++) {
      if (g_jobs[i].command == NULL || strstr(g_jobs[i].command, pattern) == NULL) {
        continue;
      }
      if (match != NULL) {
        return JOBS_LOOKUP_AMBIGUOUS;
      }
      match = &g_jobs[i];
    }
    return fill_info_from_job(match, out) ? JOBS_LOOKUP_OK : JOBS_LOOKUP_NO_MATCH;
  }

  if (spec_all_digits(spec + 1)) {
    unsigned long job_id;
    char *end;

    job_id = strtoul(spec + 1, &end, 10);
    if (*end != '\0' || job_id == 0) {
      return JOBS_LOOKUP_INVALID;
    }
    for (i = 0; i < g_job_count; i++) {
      if (g_jobs[i].job_id == (unsigned int)job_id) {
        return fill_info_from_job(&g_jobs[i], out) ? JOBS_LOOKUP_OK
                                                   : JOBS_LOOKUP_NO_MATCH;
      }
    }
    return JOBS_LOOKUP_NO_MATCH;
  }

  if (spec[1] == '\0') {
    return jobs_get_current(false, out) ? JOBS_LOOKUP_OK : JOBS_LOOKUP_NO_MATCH;
  }

  match = NULL;
  for (i = 0; i < g_job_count; i++) {
    size_t pattern_len;

    if (g_jobs[i].command == NULL) {
      continue;
    }
    pattern_len = strlen(spec + 1);
    if (strncmp(g_jobs[i].command, spec + 1, pattern_len) != 0) {
      continue;
    }
    if (match != NULL) {
      return JOBS_LOOKUP_AMBIGUOUS;
    }
    match = &g_jobs[i];
  }
  return fill_info_from_job(match, out) ? JOBS_LOOKUP_OK : JOBS_LOOKUP_NO_MATCH;
}

bool jobs_find_by_pgid(pid_t pgid, struct jobs_entry_info *out) {
  return fill_info_from_job(find_job_by_pgid(pgid), out);
}

bool jobs_find_by_pid(pid_t pid, struct jobs_entry_info *out) {
  return fill_info_from_job(find_job_by_pid(pid), out);
}

bool jobs_job_is_completed(pid_t pgid) {
  return job_all_done(find_job_by_pgid(pgid));
}

bool jobs_get_job_wait_status(pid_t pgid, int *wait_status_out) {
  struct tracked_job *job;
  struct tracked_process *proc;

  if (wait_status_out == NULL) {
    return false;
  }

  job = find_job_by_pgid(pgid);
  if (job == NULL) {
    return false;
  }
  proc = find_process(job, job->status_pid);
  if (proc == NULL || !proc->have_wait_status) {
    return false;
  }
  *wait_status_out = proc->wait_status;
  return true;
}

bool jobs_has_stopped(void) {
  size_t i;

  for (i = 0; i < g_job_count; i++) {
    if (g_jobs[i].stopped) {
      return true;
    }
  }
  return false;
}

size_t jobs_count_active(void) {
  return g_job_count;
}
