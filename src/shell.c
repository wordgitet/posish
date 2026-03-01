#define _POSIX_C_SOURCE 200809L

#include "shell.h"

#include "ast.h"
#include "error.h"
#include "exec.h"
#include "parser.h"
#include "signals.h"
#include "trace.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct heredoc_marker {
    char *delimiter;
    bool strip_tabs;
};

static void free_heredoc_markers(struct heredoc_marker *markers, size_t count) {
    size_t i;

    for (i = 0; i < count; i++) {
        free(markers[i].delimiter);
    }
    free(markers);
}

static char *unquote_heredoc_delimiter(const char *raw, size_t len) {
    size_t i;
    size_t out_len;
    char *out;

    out = malloc(len + 1);
    if (out == NULL) {
        return NULL;
    }

    out_len = 0;
    i = 0;
    while (i < len) {
        char ch;

        ch = raw[i];
        if (ch == '\\' && i + 1 < len) {
            out[out_len++] = raw[i + 1];
            i += 2;
            continue;
        }
        if (ch == '\'' || ch == '"') {
            char q;

            q = ch;
            i++;
            while (i < len && raw[i] != q) {
                if (q == '"' && raw[i] == '\\' && i + 1 < len) {
                    out[out_len++] = raw[i + 1];
                    i += 2;
                    continue;
                }
                out[out_len++] = raw[i];
                i++;
            }
            if (i < len && raw[i] == q) {
                i++;
            }
            continue;
        }
        out[out_len++] = ch;
        i++;
    }

    out[out_len] = '\0';
    return out;
}

static bool collect_heredoc_markers(const char *buf, size_t len,
                                    struct heredoc_marker **markers_out,
                                    size_t *count_out) {
    size_t i;
    char quote;
    struct heredoc_marker *markers;
    size_t count;

    i = 0;
    quote = '\0';
    markers = NULL;
    count = 0;

    while (i < len) {
        char ch;

        ch = buf[i];
        if (quote == '\0') {
            if (ch == '\\' && i + 1 < len) {
                i += 2;
                continue;
            }
            if (ch == '\'' || ch == '"') {
                quote = ch;
                i++;
                continue;
            }
            if (ch == '<' && i + 1 < len && buf[i + 1] == '<') {
                bool strip_tabs;
                size_t start;
                size_t end;
                char *delim;
                struct heredoc_marker *grown;

                i += 2;
                strip_tabs = false;
                if (i < len && buf[i] == '-') {
                    strip_tabs = true;
                    i++;
                }
                while (i < len && (buf[i] == ' ' || buf[i] == '\t')) {
                    i++;
                }
                start = i;
                while (i < len && !isspace((unsigned char)buf[i]) &&
                       buf[i] != ';' && buf[i] != '&' && buf[i] != '|' &&
                       buf[i] != '<' && buf[i] != '>') {
                    if (buf[i] == '\\' && i + 1 < len) {
                        i += 2;
                        continue;
                    }
                    if (buf[i] == '\'' || buf[i] == '"') {
                        char q;

                        q = buf[i];
                        i++;
                        while (i < len && buf[i] != q) {
                            if (q == '"' && buf[i] == '\\' && i + 1 < len) {
                                i += 2;
                                continue;
                            }
                            i++;
                        }
                        if (i < len && buf[i] == q) {
                            i++;
                        }
                        continue;
                    }
                    i++;
                }
                end = i;
                if (end <= start) {
                    continue;
                }

                delim = unquote_heredoc_delimiter(buf + start, end - start);
                if (delim == NULL) {
                    free_heredoc_markers(markers, count);
                    return false;
                }

                grown = realloc(markers, sizeof(*markers) * (count + 1));
                if (grown == NULL) {
                    free(delim);
                    free_heredoc_markers(markers, count);
                    return false;
                }
                markers = grown;
                markers[count].delimiter = delim;
                markers[count].strip_tabs = strip_tabs;
                count++;
                continue;
            }
        } else if (quote == '\'' && ch == '\'') {
            quote = '\0';
        } else if (quote == '"') {
            if (ch == '\\' && i + 1 < len) {
                i += 2;
                continue;
            }
            if (ch == '"') {
                quote = '\0';
            }
        }

        i++;
    }

    *markers_out = markers;
    *count_out = count;
    return true;
}

static bool heredoc_needs_more_input(const char *buf, size_t len) {
    struct heredoc_marker *markers;
    size_t marker_count;
    size_t i;
    size_t pos;

    markers = NULL;
    marker_count = 0;
    if (!collect_heredoc_markers(buf, len, &markers, &marker_count)) {
        return true;
    }
    if (marker_count == 0) {
        return false;
    }

    pos = 0;
    while (pos < len && buf[pos] != '\n') {
        pos++;
    }
    if (pos >= len) {
        free_heredoc_markers(markers, marker_count);
        return true;
    }
    pos++;

    for (i = 0; i < marker_count; i++) {
        bool found;

        found = false;
        while (pos <= len) {
            size_t line_start;
            size_t line_end;
            size_t cmp_start;
            size_t delim_len;

            line_start = pos;
            while (pos < len && buf[pos] != '\n') {
                pos++;
            }
            line_end = pos;
            cmp_start = line_start;
            if (markers[i].strip_tabs) {
                while (cmp_start < line_end && buf[cmp_start] == '\t') {
                    cmp_start++;
                }
            }

            delim_len = strlen(markers[i].delimiter);
            if (line_end - cmp_start == delim_len &&
                strncmp(buf + cmp_start, markers[i].delimiter, delim_len) == 0) {
                found = true;
                if (pos < len) {
                    pos++;
                }
                break;
            }

            if (pos >= len) {
                break;
            }
            pos++;
        }

        if (!found) {
            free_heredoc_markers(markers, marker_count);
            return true;
        }
    }

    free_heredoc_markers(markers, marker_count);
    return false;
}

static int needs_more_input(char *buf, size_t *len) {
    size_t i;
    int quote;

    quote = 0;
    i = 0;
    while (i < *len) {
        char ch;

        ch = buf[i];
        if (quote == 0) {
            if (ch == '\'') {
                quote = '\'';
                i++;
                continue;
            }
            if (ch == '"') {
                quote = '"';
                i++;
                continue;
            }
            if (ch == '\\' && i + 1 < *len && buf[i + 1] == '\n') {
                memmove(&buf[i], &buf[i + 2], *len - (i + 1));
                *len -= 2;
                buf[*len] = '\0';
                return 1;
            }
            if (ch == '\\' && i + 1 < *len) {
                i += 2;
                continue;
            }
            i++;
            continue;
        }

        if (quote == '\'' && ch == '\'') {
            quote = 0;
            i++;
            continue;
        }
        if (quote == '"' && ch == '\\' && i + 1 < *len) {
            i += 2;
            continue;
        }
        if (quote == '"' && ch == '"') {
            quote = 0;
            i++;
            continue;
        }
        i++;
    }

    if (quote != 0) {
        return 1;
    }
    return heredoc_needs_more_input(buf, *len) ? 1 : 0;
}

static void append_command(char **buf, size_t *len, size_t *cap,
                           const char *line) {
    size_t n;

    n = strlen(line);
    if (*len + n + 1 > *cap) {
        size_t new_cap;

        new_cap = *cap == 0 ? 128 : *cap;
        while (*len + n + 1 > new_cap) {
            new_cap *= 2;
        }
        *buf = realloc(*buf, new_cap);
        *cap = new_cap;
    }

    memcpy(*buf + *len, line, n);
    *len += n;
    (*buf)[*len] = '\0';
}

static bool inherited_ignore_locked(const struct shell_state *state, int signo) {
    return !state->interactive && signals_inherited_ignored(signo) &&
           !state->parent_was_interactive;
}

static bool trap_clear_keeps_ignore(const struct shell_state *state, int signo) {
    if (state->interactive && state->main_context && signals_policy_ignored(signo)) {
        return true;
    }

    return inherited_ignore_locked(state, signo);
}

void shell_state_init(struct shell_state *state) {
    int signo;

    state->last_status = 0;
    state->last_cmdsub_status = 0;
    state->cmdsub_performed = false;
    /* Keep $$ stable across subshell/cmdsub contexts for POSIX semantics. */
    state->shell_pid = getpid();
    state->errexit = false;
    state->should_exit = false;
    state->exit_status = 0;
    state->last_handled_signal = 0;
    state->interactive = false;
    state->explicit_non_interactive = false;
    state->parent_was_interactive = false;
    state->monitor_mode = false;
    state->in_async_context = false;
    state->main_context = true;
    state->last_async_pid = -1;
    state->exit_trap = NULL;
    state->running_exit_trap = false;
    state->running_signal_trap = false;
    for (signo = 0; signo < NSIG; signo++) {
        state->signal_traps[signo] = NULL;
        state->signal_cleared[signo] = false;
    }
    state->readonly_names = NULL;
    state->readonly_count = 0;
    state->functions = NULL;
    state->function_count = 0;
    state->positional_params = NULL;
    state->positional_count = 0;
}

void shell_state_destroy(struct shell_state *state) {
    size_t i;
    int signo;

    for (i = 0; i < state->readonly_count; i++) {
        free(state->readonly_names[i]);
    }
    free(state->readonly_names);

    for (i = 0; i < state->function_count; i++) {
        free(state->functions[i].name);
        free(state->functions[i].body);
    }
    free(state->functions);

    for (i = 0; i < state->positional_count; i++) {
        free(state->positional_params[i]);
    }
    free(state->positional_params);
    free(state->exit_trap);
    for (signo = 0; signo < NSIG; signo++) {
        free(state->signal_traps[signo]);
        state->signal_traps[signo] = NULL;
        state->signal_cleared[signo] = false;
    }

    state->readonly_names = NULL;
    state->readonly_count = 0;
    state->functions = NULL;
    state->function_count = 0;
    state->positional_params = NULL;
    state->positional_count = 0;
    state->explicit_non_interactive = false;
    state->parent_was_interactive = false;
    state->in_async_context = false;
    state->main_context = true;
    state->last_async_pid = -1;
    state->exit_trap = NULL;
    state->running_exit_trap = false;
    state->running_signal_trap = false;
    state->last_handled_signal = 0;
}

void shell_refresh_signal_policy(struct shell_state *state) {
    int signo;

    /*
     * Keep runtime option flips (set +/-i, set +/-m) and trap state in one
     * place so all execution paths observe the same live dispositions.
     */
    signals_apply_policy(state->interactive, state->monitor_mode);

    for (signo = 1; signo < NSIG; signo++) {
        if (state->signal_traps[signo] != NULL) {
            if (state->signal_traps[signo][0] == '\0') {
                (void)signals_set_ignored(signo);
            } else if (inherited_ignore_locked(state, signo)) {
                free(state->signal_traps[signo]);
                state->signal_traps[signo] = NULL;
                (void)signals_set_ignored(signo);
            } else {
                (void)signals_set_trap(signo);
            }
        } else if (state->signal_cleared[signo]) {
            if (trap_clear_keeps_ignore(state, signo)) {
                (void)signals_set_ignored(signo);
            } else {
                (void)signals_set_default(signo);
            }
        }
        signals_clear_pending(signo);
    }
}

void shell_run_pending_traps(struct shell_state *state) {
    int signo;

    if (state->running_signal_trap || state->running_exit_trap) {
        return;
    }

    state->running_signal_trap = true;
    for (;;) {
        int saved_last_status;
        bool saved_should_exit;
        int saved_exit_status;
        char *command;

        signo = signals_take_next_pending();
        if (signo <= 0 || signo >= NSIG) {
            break;
        }

        state->last_handled_signal = signo;
        command = state->signal_traps[signo];
        if (command == NULL || command[0] == '\0') {
            trace_log(POSISH_TRACE_TRAPS,
                      "pending signal=%d has no command trap", signo);
            continue;
        }

        trace_log(POSISH_TRACE_TRAPS, "run signal trap signo=%d command=%s",
                  signo, command);

        saved_last_status = state->last_status;
        saved_should_exit = state->should_exit;
        saved_exit_status = state->exit_status;

        state->should_exit = false;
        (void)shell_run_command(state, command);
        trace_log(POSISH_TRACE_TRAPS,
                  "signal trap signo=%d finished status=%d should_exit=%d",
                  signo, state->last_status, state->should_exit ? 1 : 0);
        if (!state->should_exit) {
            state->should_exit = saved_should_exit;
            state->exit_status = saved_exit_status;
        }
        state->last_status = saved_last_status;
    }
    state->running_signal_trap = false;
}

void shell_run_exit_trap(struct shell_state *state) {
    int saved_last_status;
    int saved_exit_status;
    bool saved_should_exit;

    if (state->running_exit_trap || state->exit_trap == NULL) {
        return;
    }

    saved_last_status = state->last_status;
    saved_should_exit = state->should_exit;
    saved_exit_status = state->exit_status;

    state->running_exit_trap = true;
    state->should_exit = false;
    trace_log(POSISH_TRACE_TRAPS, "run EXIT trap command=%s", state->exit_trap);
    (void)shell_run_command(state, state->exit_trap);
    state->running_exit_trap = false;

    /* Keep the original shell exit unless the trap explicitly requested one. */
    if (!state->should_exit) {
        state->should_exit = saved_should_exit;
        state->exit_status = saved_exit_status;
        state->last_status = saved_last_status;
    }
}

int shell_run_command(struct shell_state *state, const char *command) {
    struct ast_program *program;
    const char *p;
    int status;

    p = command;
    while (*p == ' ' || *p == '\t' || *p == '\n') {
        p++;
    }
    if (*p == '\0') {
        shell_run_pending_traps(state);
        return state->last_status;
    }

    trace_log(POSISH_TRACE_TRAPS, "shell_run_command: %s", command);
    shell_run_pending_traps(state);

    if (parse_program(command, &program) != 0) {
        state->last_status = 2;
        return state->last_status;
    }

    status = exec_run_program(state, program);
    ast_program_free(program);

    state->last_status = status;
    trace_log(POSISH_TRACE_TRAPS, "command finished status=%d", status);
    shell_run_pending_traps(state);
    return status;
}

int shell_run_stream(struct shell_state *state, FILE *stream, bool interactive) {
    char *line;
    char *command;
    size_t cap;
    size_t command_len;
    size_t command_cap;
    bool secondary_prompt;
    bool line_mode_input;
    size_t line_no;

    line = NULL;
    cap = 0;
    command = NULL;
    command_len = 0;
    command_cap = 0;
    secondary_prompt = false;
    line_mode_input =
        (interactive && isatty(fileno(stream))) ||
        (!interactive && state->monitor_mode &&
         !state->explicit_non_interactive);
    line_no = 1;
    state->interactive = interactive;

    if (line_mode_input && !interactive) {
        /*
         * Keep stdin position synchronized with child jobs in monitor-mode
         * test runs. Buffered stdio prefetch can otherwise consume future
         * script lines before background commands read from fd 0.
         */
        (void)setvbuf(stream, NULL, _IONBF, 0);
    }

    if (!line_mode_input) {
        ssize_t nread;

        /*
         * Batch whole-script input whenever stdin is not a TTY.
         * This keeps here-doc/compound syntax intact even for `-i` test runs
         * that intentionally force interactive signal semantics on file input.
         */
        while ((nread = getline(&line, &cap, stream)) >= 0) {
            (void)nread;
            append_command(&command, &command_len, &command_cap, line);
            line_no++;
        }
        if (command_len > 0) {
            shell_run_command(state, command);
        }
        shell_run_pending_traps(state);

        free(line);
        free(command);
        if (state->should_exit) {
            return state->exit_status;
        }
        return state->last_status;
    }

    while (!state->should_exit) {
        ssize_t nread;

        shell_run_pending_traps(state);
        if (state->should_exit) {
            break;
        }

        if (line_mode_input && isatty(STDOUT_FILENO)) {
            fputs(secondary_prompt ? "> " : "posish$ ", stdout);
            fflush(stdout);
        }

        nread = getline(&line, &cap, stream);
        if (nread < 0) {
            break;
        }

        append_command(&command, &command_len, &command_cap, line);
        if (needs_more_input(command, &command_len)) {
            secondary_prompt = true;
            line_no++;
            continue;
        }

        shell_run_command(state, command);
        command_len = 0;
        if (command != NULL) {
            command[0] = '\0';
        }
        secondary_prompt = false;
        line_no++;
    }

    if (command_len > 0 && needs_more_input(command, &command_len)) {
        posish_error_at("<input>", line_no, 1, "syntax",
                        "unexpected EOF while looking for matching quote");
        state->last_status = 2;
    }

    free(line);
    free(command);

    if (state->should_exit) {
        return state->exit_status;
    }

    return state->last_status;
}

int shell_run_file(struct shell_state *state, const char *path) {
    FILE *fp;
    int status;

    fp = fopen(path, "r");
    if (fp == NULL) {
        perror(path);
        return 1;
    }

    status = shell_run_stream(state, fp, false);
    fclose(fp);
    return status;
}
