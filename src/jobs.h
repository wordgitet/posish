/* SPDX-License-Identifier: 0BSD */

/* posish - job interface */

#ifndef POSISH_JOBS_H
#define POSISH_JOBS_H

#include <stddef.h>
#include <stdbool.h>
#include <sys/types.h>

struct jobs_entry_info {
    pid_t pgid;
    pid_t status_pid;
    unsigned int job_id;
    const char *command;
    bool stopped;
};

enum jobs_lookup_result {
    JOBS_LOOKUP_OK = 0,
    JOBS_LOOKUP_NO_MATCH,
    JOBS_LOOKUP_AMBIGUOUS,
    JOBS_LOOKUP_INVALID
};

void jobs_init(void);
void jobs_destroy(void);

void jobs_track_async(pid_t pid, pid_t pgid, const char *command);
void jobs_track_job(pid_t pgid, const pid_t *pids, size_t count, pid_t status_pid,
                    const char *command, bool stopped);
void jobs_note_process_status(pid_t pid, int wait_status);
void jobs_mark_job_running(pid_t pgid);
void jobs_forget_pgid(pid_t pgid);
bool jobs_get_current(bool stopped_only, struct jobs_entry_info *out);
bool jobs_get_previous(bool stopped_only, struct jobs_entry_info *out);
enum jobs_lookup_result jobs_get_by_spec(const char *spec,
                                         struct jobs_entry_info *out);
bool jobs_find_by_pgid(pid_t pgid, struct jobs_entry_info *out);
bool jobs_find_by_pid(pid_t pid, struct jobs_entry_info *out);
bool jobs_job_is_completed(pid_t pgid);
bool jobs_get_job_wait_status(pid_t pgid, int *wait_status_out);
bool jobs_has_stopped(void);
size_t jobs_count_active(void);

#endif
