/* SPDX-License-Identifier: 0BSD */

/* posish - job handling */

#include "jobs.h"
#include "trace.h"

#include <stdlib.h>
#include <string.h>

struct tracked_job {
  pid_t pid;
  pid_t pgid;
  unsigned int job_id;
  unsigned long serial;
  bool stopped;
  char *command;
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

  copy = malloc(end - start + 1);
  if (copy == NULL) {
    return NULL;
  }
  memcpy(copy, command + start, end - start);
  copy[end - start] = '\0';
  return copy;
}

static bool job_matches_spec(const struct tracked_job *job, const char *spec) {
  const char *pattern;
  size_t pattern_len;

  if (job == NULL || spec == NULL || spec[0] != '%') {
    return false;
  }

  if (spec[1] == '\0') {
    return true;
  }

  if (job->command == NULL) {
    return false;
  }

  if (spec[1] == '?') {
    pattern = spec + 2;
    if (pattern[0] == '\0') {
      return true;
    }
    return strstr(job->command, pattern) != NULL;
  }

  pattern = spec + 1;
  pattern_len = strlen(pattern);
  return strncmp(job->command, pattern, pattern_len) == 0;
}

static struct tracked_job *find_job(pid_t pid) {
  size_t i;

  for (i = 0; i < g_job_count; i++) {
    if (g_jobs[i].pid == pid) {
      return &g_jobs[i];
    }
  }
  return NULL;
}

static void touch_job(struct tracked_job *job) {
  if (job != NULL) {
    job->serial = g_next_serial++;
  }
}

static struct tracked_job *add_job(pid_t pid, pid_t pgid, const char *command,
                                   bool stopped) {
  struct tracked_job *new_jobs;
  struct tracked_job *job;

  new_jobs = realloc(g_jobs, sizeof(*g_jobs) * (g_job_count + 1));
  if (new_jobs == NULL) {
    return NULL;
  }
  g_jobs = new_jobs;

  job = &g_jobs[g_job_count];
  job->pid = pid;
  job->pgid = pgid > 0 ? pgid : pid;
  job->job_id = g_next_job_id++;
  job->serial = g_next_serial++;
  job->stopped = stopped;
  job->command = dup_trimmed_command(command);
  g_job_count++;
  return job;
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

  free(job->command);
  job->command = copy;
}

static bool fill_info_from_job(const struct tracked_job *job,
                               struct jobs_entry_info *out) {
  if (job == NULL || out == NULL) {
    return false;
  }

  out->pid = job->pid;
  out->pgid = job->pgid;
  out->job_id = job->job_id;
  out->command = job->command;
  out->stopped = job->stopped;
  return true;
}

static bool get_best_job(bool stopped_only, struct jobs_entry_info *out) {
  size_t i;
  struct tracked_job *best;

  best = NULL;
  for (i = 0; i < g_job_count; i++) {
    if (stopped_only && !g_jobs[i].stopped) {
      continue;
    }
    if (best == NULL || g_jobs[i].serial > best->serial) {
      best = &g_jobs[i];
    }
  }

  return fill_info_from_job(best, out);
}

void jobs_init(void) {
  size_t i;

  for (i = 0; i < g_job_count; i++) {
    free(g_jobs[i].command);
  }
  free(g_jobs);
  g_jobs = NULL;
  g_job_count = 0;
  g_next_job_id = 1;
  g_next_serial = 1;
  trace_log(POSISH_TRACE_JOBS, "jobs init");
}

void jobs_destroy(void) {
  size_t i;

  for (i = 0; i < g_job_count; i++) {
    free(g_jobs[i].command);
  }
  free(g_jobs);
  g_jobs = NULL;
  g_job_count = 0;
  g_next_job_id = 1;
  g_next_serial = 1;
}

void jobs_track_async(pid_t pid, pid_t pgid, const char *command) {
  struct tracked_job *job;

  if (pid <= 0) {
    return;
  }

  job = find_job(pid);
  if (job == NULL) {
    job = add_job(pid, pgid, command, false);
    if (job == NULL) {
      return;
    }
  } else {
    job->stopped = false;
    if (pgid > 0) {
      job->pgid = pgid;
    }
    set_job_command(job, command);
    touch_job(job);
  }

  trace_log(POSISH_TRACE_JOBS, "track async pid=%ld pgid=%ld command=%s",
            (long)pid, (long)job->pgid,
            job->command == NULL ? "<none>" : job->command);
}

void jobs_forget(pid_t pid) {
  size_t i;

  if (pid <= 0) {
    return;
  }

  for (i = 0; i < g_job_count; i++) {
    if (g_jobs[i].pid == pid) {
      free(g_jobs[i].command);
      g_jobs[i] = g_jobs[g_job_count - 1];
      g_job_count--;
      if (g_job_count == 0) {
        free(g_jobs);
        g_jobs = NULL;
      } else {
        struct tracked_job *shrunk;

        shrunk = realloc(g_jobs, sizeof(*g_jobs) * g_job_count);
        if (shrunk != NULL) {
          g_jobs = shrunk;
        }
      }
      trace_log(POSISH_TRACE_JOBS, "forget pid=%ld", (long)pid);
      return;
    }
  }
}

pid_t jobs_resolve_spec(const char *spec) {
  struct jobs_entry_info info;

  if (spec == NULL || spec[0] != '%') {
    return -1;
  }

  if (!jobs_get_by_spec(spec, &info)) {
    trace_log(POSISH_TRACE_JOBS, "resolve spec=%s pid=<none>", spec);
    return -1;
  }
  trace_log(POSISH_TRACE_JOBS, "resolve spec=%s pid=%ld", spec, (long)info.pid);
  return info.pid;
}

void jobs_note_stopped(pid_t pid, pid_t pgid) {
  jobs_note_stopped_with_command(pid, pgid, NULL);
}

void jobs_note_stopped_with_command(pid_t pid, pid_t pgid,
                                    const char *command) {
  struct tracked_job *job;

  if (pid > 0) {
    job = find_job(pid);
    if (job == NULL) {
      job = add_job(pid, pgid, command, true);
      if (job == NULL) {
        return;
      }
    } else {
      job->stopped = true;
      if (pgid > 0) {
        job->pgid = pgid;
      }
      set_job_command(job, command);
      touch_job(job);
    }
    trace_log(POSISH_TRACE_JOBS, "note stopped pid=%ld pgid=%ld", (long)pid,
              (long)job->pgid);
  }
}

void jobs_mark_running(pid_t pid) {
  struct tracked_job *job;

  job = find_job(pid);
  if (job == NULL) {
    return;
  }
  job->stopped = false;
  touch_job(job);
  trace_log(POSISH_TRACE_JOBS, "mark running pid=%ld", (long)pid);
}

bool jobs_get_current(bool stopped_only, struct jobs_entry_info *out) {
  bool ok;

  ok = get_best_job(stopped_only, out);
  if (ok) {
    trace_log(POSISH_TRACE_JOBS,
              "current stopped_only=%d pid=%ld pgid=%ld id=%u stopped=%d",
              stopped_only ? 1 : 0, (long)out->pid, (long)out->pgid,
              out->job_id, out->stopped ? 1 : 0);
  } else {
    trace_log(POSISH_TRACE_JOBS, "current stopped_only=%d <none>",
              stopped_only ? 1 : 0);
  }
  return ok;
}

bool jobs_get_by_spec(const char *spec, struct jobs_entry_info *out) {
  size_t i;
  struct tracked_job *best;

  if (spec == NULL || spec[0] != '%') {
    return false;
  }

  if (spec[1] == '\0') {
    return get_best_job(false, out);
  }

  best = NULL;
  for (i = 0; i < g_job_count; i++) {
    if (!job_matches_spec(&g_jobs[i], spec)) {
      continue;
    }
    if (best == NULL || g_jobs[i].serial > best->serial) {
      best = &g_jobs[i];
    }
  }
  return fill_info_from_job(best, out);
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
