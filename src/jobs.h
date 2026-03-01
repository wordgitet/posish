#ifndef POSISH_JOBS_H
#define POSISH_JOBS_H

#include <stdbool.h>
#include <sys/types.h>

struct jobs_entry_info {
    pid_t pid;
    unsigned int job_id;
    const char *command;
    bool stopped;
};

void jobs_init(void);
void jobs_destroy(void);

void jobs_track_async(pid_t pid, const char *command);
void jobs_forget(pid_t pid);
pid_t jobs_resolve_spec(const char *spec);

void jobs_note_stopped(pid_t pid);
void jobs_note_stopped_with_command(pid_t pid, const char *command);
void jobs_mark_running(pid_t pid);
bool jobs_get_current(bool stopped_only, struct jobs_entry_info *out);
bool jobs_get_by_spec(const char *spec, struct jobs_entry_info *out);
bool jobs_has_stopped(void);

#endif
