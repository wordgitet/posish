#ifndef POSISH_JOBS_H
#define POSISH_JOBS_H

#include <stdbool.h>
#include <sys/types.h>

void jobs_init(void);
void jobs_destroy(void);

void jobs_track_async(pid_t pid, const char *command);
void jobs_forget(pid_t pid);
pid_t jobs_resolve_spec(const char *spec);

void jobs_note_stopped(pid_t pid);
pid_t jobs_take_stopped(void);
bool jobs_has_stopped(void);

#endif
