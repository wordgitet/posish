#include "jobs.h"
#include "trace.h"

#include <stdlib.h>
#include <string.h>

static pid_t g_last_stopped_pid;

struct tracked_job {
    pid_t pid;
    char *command;
};

static struct tracked_job *g_jobs;
static size_t g_job_count;

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
    while (end > start &&
           (command[end - 1] == ' ' || command[end - 1] == '\t' ||
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

void jobs_init(void) {
    size_t i;

    for (i = 0; i < g_job_count; i++) {
        free(g_jobs[i].command);
    }
    free(g_jobs);
    g_jobs = NULL;
    g_job_count = 0;

    /* M1 job-control shim: keep one resumable foreground-stopped process. */
    g_last_stopped_pid = -1;
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
    g_last_stopped_pid = -1;
}

void jobs_track_async(pid_t pid, const char *command) {
    struct tracked_job *new_jobs;
    char *copy;

    if (pid <= 0) {
        return;
    }

    copy = dup_trimmed_command(command);
    new_jobs = realloc(g_jobs, sizeof(*g_jobs) * (g_job_count + 1));
    if (new_jobs == NULL) {
        free(copy);
        return;
    }

    g_jobs = new_jobs;
    g_jobs[g_job_count].pid = pid;
    g_jobs[g_job_count].command = copy;
    g_job_count++;

    trace_log(POSISH_TRACE_JOBS, "track async pid=%ld command=%s", (long)pid,
              copy == NULL ? "<oom>" : copy);
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
    size_t i;

    if (spec == NULL || spec[0] != '%') {
        return -1;
    }

    for (i = g_job_count; i > 0; i--) {
        if (job_matches_spec(&g_jobs[i - 1], spec)) {
            trace_log(POSISH_TRACE_JOBS, "resolve spec=%s pid=%ld", spec,
                      (long)g_jobs[i - 1].pid);
            return g_jobs[i - 1].pid;
        }
    }

    trace_log(POSISH_TRACE_JOBS, "resolve spec=%s pid=<none>", spec);
    return -1;
}

void jobs_note_stopped(pid_t pid) {
    if (pid > 0) {
        g_last_stopped_pid = pid;
        trace_log(POSISH_TRACE_JOBS, "note stopped pid=%ld", (long)pid);
    }
}

pid_t jobs_take_stopped(void) {
    pid_t pid;

    pid = g_last_stopped_pid;
    g_last_stopped_pid = -1;
    trace_log(POSISH_TRACE_JOBS, "take stopped pid=%ld", (long)pid);
    return pid;
}

bool jobs_has_stopped(void) {
    return g_last_stopped_pid > 0;
}
