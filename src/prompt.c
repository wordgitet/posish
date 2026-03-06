/* SPDX-License-Identifier: 0BSD */

/* posish - prompt expansion */

#include "prompt.h"

#include "arena.h"
#include "jobs.h"
#include "path.h"
#include "shell.h"
#include "vars.h"

#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int append_bytes(char **buf, size_t *len, size_t *cap,
                        const char *text, size_t text_len) {
    char *grown;

    if (text_len == 0) {
        return 0;
    }

    if (*len + text_len + 1 > *cap) {
        size_t new_cap;

        new_cap = (*cap == 0) ? 64 : *cap;
        while (*len + text_len + 1 > new_cap) {
            new_cap *= 2;
        }
        grown = arena_realloc_in(NULL, *buf, new_cap);
        if (grown == NULL) {
            return 1;
        }
        *buf = grown;
        *cap = new_cap;
    }

    memcpy(*buf + *len, text, text_len);
    *len += text_len;
    (*buf)[*len] = '\0';
    return 0;
}

static int append_cstr(char **buf, size_t *len, size_t *cap, const char *text) {
    if (text == NULL) {
        return 0;
    }
    return append_bytes(buf, len, cap, text, strlen(text));
}

static int append_char(char **buf, size_t *len, size_t *cap, char ch) {
    return append_bytes(buf, len, cap, &ch, 1);
}

static const char *basename_cstr(const char *path) {
    const char *slash;

    if (path == NULL || path[0] == '\0') {
        return "posish";
    }
    slash = strrchr(path, '/');
    if (slash == NULL || slash[1] == '\0') {
        return path;
    }
    return slash + 1;
}

static int append_unsigned_value(char **buf, size_t *len, size_t *cap,
                                 unsigned long value) {
    char num[64];

    snprintf(num, sizeof(num), "%lu", value);
    return append_cstr(buf, len, cap, num);
}

static int append_pid_value(char **buf, size_t *len, size_t *cap, pid_t value) {
    char num[64];

    if (value <= 0) {
        return 0;
    }
    snprintf(num, sizeof(num), "%ld", (long)value);
    return append_cstr(buf, len, cap, num);
}

static int append_special_param(char **buf, size_t *len, size_t *cap,
                                struct shell_state *state, char param) {
    switch (param) {
    case '?':
        return append_unsigned_value(buf, len, cap,
                                     (unsigned long)state->last_status);
    case '$':
        return append_pid_value(buf, len, cap, state->shell_pid);
    case '!':
        return append_pid_value(buf, len, cap, state->last_async_pid);
    case '#':
        return append_unsigned_value(buf, len, cap,
                                     (unsigned long)state->positional_count);
    case '0': {
        const char *value;

        value = getenv("0");
        return append_cstr(buf, len, cap, value == NULL ? "" : value);
    }
    default:
        return append_char(buf, len, cap, '$');
    }
}

static int prompt_expand_simple_params(const char *in, struct shell_state *state,
                                       char **out) {
    char *buf;
    size_t len;
    size_t cap;
    size_t i;

    buf = NULL;
    len = 0;
    cap = 0;
    for (i = 0; in[i] != '\0';) {
        if (in[i] != '$') {
            if (append_char(&buf, &len, &cap, in[i]) != 0) {
                arena_maybe_free(buf);
                return 1;
            }
            i++;
            continue;
        }

        if (in[i + 1] == '\0') {
            if (append_char(&buf, &len, &cap, '$') != 0) {
                arena_maybe_free(buf);
                return 1;
            }
            break;
        }

        if (strchr("?$!#0", in[i + 1]) != NULL) {
            if (append_special_param(&buf, &len, &cap, state, in[i + 1]) != 0) {
                arena_maybe_free(buf);
                return 1;
            }
            i += 2;
            continue;
        }

        if ((in[i + 1] >= 'A' && in[i + 1] <= 'Z') ||
            (in[i + 1] >= 'a' && in[i + 1] <= 'z') || in[i + 1] == '_') {
            size_t j;
            char *name;
            const char *value;

            j = i + 2;
            while ((in[j] >= 'A' && in[j] <= 'Z') ||
                   (in[j] >= 'a' && in[j] <= 'z') ||
                   (in[j] >= '0' && in[j] <= '9') || in[j] == '_') {
                j++;
            }
            name = arena_alloc_in(NULL, j - (i + 1) + 1);
            if (name == NULL) {
                arena_maybe_free(buf);
                return 1;
            }
            memcpy(name, in + i + 1, j - (i + 1));
            name[j - (i + 1)] = '\0';
            value = getenv(name);
            arena_maybe_free(name);
            if (append_cstr(&buf, &len, &cap, value == NULL ? "" : value) != 0) {
                arena_maybe_free(buf);
                return 1;
            }
            i = j;
            continue;
        }

        if (in[i + 1] == '{') {
            size_t j;
            bool special;

            special = false;
            j = i + 2;
            if (strchr("?$!#0", in[j]) != NULL) {
                special = true;
                j++;
            } else if ((in[j] >= 'A' && in[j] <= 'Z') ||
                       (in[j] >= 'a' && in[j] <= 'z') || in[j] == '_') {
                char *name;
                const char *value;

                j++;
                while ((in[j] >= 'A' && in[j] <= 'Z') ||
                       (in[j] >= 'a' && in[j] <= 'z') ||
                       (in[j] >= '0' && in[j] <= '9') || in[j] == '_') {
                    j++;
                }
                if (in[j] == '}') {
                    name = arena_alloc_in(NULL, j - (i + 2) + 1);
                    if (name == NULL) {
                        arena_maybe_free(buf);
                        return 1;
                    }
                    memcpy(name, in + i + 2, j - (i + 2));
                    name[j - (i + 2)] = '\0';
                    value = getenv(name);
                    arena_maybe_free(name);
                    if (append_cstr(&buf, &len, &cap,
                                    value == NULL ? "" : value) != 0) {
                        arena_maybe_free(buf);
                        return 1;
                    }
                    i = j + 1;
                    continue;
                }
            }

            if (special && in[j] == '}') {
                if (append_special_param(&buf, &len, &cap, state, in[i + 2]) != 0) {
                    arena_maybe_free(buf);
                    return 1;
                }
                i = j + 1;
                continue;
            }
        }

        if (append_char(&buf, &len, &cap, '$') != 0) {
            arena_maybe_free(buf);
            return 1;
        }
        i++;
    }

    if (buf == NULL) {
        buf = arena_xstrdup("");
    }
    *out = buf;
    return 0;
}

static int append_username(char **buf, size_t *len, size_t *cap) {
    const char *name;
    struct passwd *pw;
    char num[64];

    name = getenv("LOGNAME");
    if (name == NULL || name[0] == '\0') {
        name = getenv("USER");
    }
    if (name != NULL && name[0] != '\0') {
        return append_cstr(buf, len, cap, name);
    }

    pw = getpwuid(geteuid());
    if (pw != NULL && pw->pw_name != NULL && pw->pw_name[0] != '\0') {
        return append_cstr(buf, len, cap, pw->pw_name);
    }

    snprintf(num, sizeof(num), "%ld", (long)geteuid());
    return append_cstr(buf, len, cap, num);
}

static int append_hostname(char **buf, size_t *len, size_t *cap, bool short_name) {
    char host[256];
    const char *text;
    char *dot;

    if (gethostname(host, sizeof(host)) != 0) {
        return 0;
    }
    host[sizeof(host) - 1] = '\0';
    if (short_name) {
        dot = strchr(host, '.');
        if (dot != NULL) {
            *dot = '\0';
        }
    }
    text = host;
    return append_cstr(buf, len, cap, text);
}

static char *prompt_shorten_home(const char *cwd) {
    const char *home;
    size_t cwd_len;
    size_t home_len;
    char *shortened;

    home = getenv("HOME");
    if (cwd == NULL) {
        return arena_xstrdup("");
    }
    if (home == NULL || home[0] == '\0') {
        return arena_xstrdup(cwd);
    }

    cwd_len = strlen(cwd);
    home_len = strlen(home);
    if (strcmp(cwd, home) == 0) {
        return arena_xstrdup("~");
    }
    if (cwd_len > home_len && strncmp(cwd, home, home_len) == 0 &&
        cwd[home_len] == '/') {
        shortened = arena_alloc_in(NULL, cwd_len - home_len + 2);
        if (shortened == NULL) {
            return NULL;
        }
        shortened[0] = '~';
        memcpy(shortened + 1, cwd + home_len, cwd_len - home_len + 1);
        return shortened;
    }
    return arena_xstrdup(cwd);
}

static int append_cwd(char **buf, size_t *len, size_t *cap, bool basename_only) {
    const char *pwd;
    char *cwd;
    char *display;
    const char *base;
    int rc;

    pwd = getenv("PWD");
    cwd = NULL;
    display = NULL;
    rc = 0;
    if (pwd == NULL || pwd[0] == '\0') {
        cwd = path_getcwd_alloc();
        pwd = cwd;
    }

    display = prompt_shorten_home(pwd == NULL ? "" : pwd);
    if (display == NULL) {
        arena_maybe_free(cwd);
        return 1;
    }

    if (basename_only) {
        if (strcmp(display, "/") == 0 || strcmp(display, "~") == 0) {
            rc = append_cstr(buf, len, cap, display);
        } else {
            base = basename_cstr(display);
            rc = append_cstr(buf, len, cap, base);
        }
    } else {
        rc = append_cstr(buf, len, cap, display);
    }

    arena_maybe_free(display);
    arena_maybe_free(cwd);
    return rc;
}

static int append_time_format(char **buf, size_t *len, size_t *cap,
                              const char *fmt) {
    time_t now;
    struct tm tm_now;
    char text[128];

    now = time(NULL);
    if (localtime_r(&now, &tm_now) == NULL) {
        return 0;
    }
    if (strftime(text, sizeof(text), fmt, &tm_now) == 0) {
        return 0;
    }
    return append_cstr(buf, len, cap, text);
}

static int prompt_expand_backslash_escapes(const char *in,
                                           struct shell_state *state,
                                           char **out) {
    char *buf;
    size_t len;
    size_t cap;
    size_t i;

    buf = NULL;
    len = 0;
    cap = 0;
    for (i = 0; in[i] != '\0'; i++) {
        if (in[i] != '\\') {
            if (append_char(&buf, &len, &cap, in[i]) != 0) {
                arena_maybe_free(buf);
                return 1;
            }
            continue;
        }

        i++;
        if (in[i] == '\0') {
            if (append_char(&buf, &len, &cap, '\\') != 0) {
                arena_maybe_free(buf);
                return 1;
            }
            break;
        }

        switch (in[i]) {
        case '\\':
            if (append_char(&buf, &len, &cap, '\\') != 0) {
                arena_maybe_free(buf);
                return 1;
            }
            break;
        case 'a':
            if (append_char(&buf, &len, &cap, '\a') != 0) {
                arena_maybe_free(buf);
                return 1;
            }
            break;
        case 'e':
            if (append_char(&buf, &len, &cap, '\033') != 0) {
                arena_maybe_free(buf);
                return 1;
            }
            break;
        case 'n':
            if (append_char(&buf, &len, &cap, '\n') != 0) {
                arena_maybe_free(buf);
                return 1;
            }
            break;
        case 'r':
            if (append_char(&buf, &len, &cap, '\r') != 0) {
                arena_maybe_free(buf);
                return 1;
            }
            break;
        case 'u':
            if (append_username(&buf, &len, &cap) != 0) {
                arena_maybe_free(buf);
                return 1;
            }
            break;
        case 'h':
            if (append_hostname(&buf, &len, &cap, true) != 0) {
                arena_maybe_free(buf);
                return 1;
            }
            break;
        case 'H':
            if (append_hostname(&buf, &len, &cap, false) != 0) {
                arena_maybe_free(buf);
                return 1;
            }
            break;
        case 's':
            if (append_cstr(&buf, &len, &cap,
                            state->shell_name == NULL ? "posish"
                                                      : state->shell_name) != 0) {
                arena_maybe_free(buf);
                return 1;
            }
            break;
        case 'w':
            if (append_cwd(&buf, &len, &cap, false) != 0) {
                arena_maybe_free(buf);
                return 1;
            }
            break;
        case 'W':
            if (append_cwd(&buf, &len, &cap, true) != 0) {
                arena_maybe_free(buf);
                return 1;
            }
            break;
        case 'd':
            if (append_time_format(&buf, &len, &cap, "%a %b %d") != 0) {
                arena_maybe_free(buf);
                return 1;
            }
            break;
        case 't':
            if (append_time_format(&buf, &len, &cap, "%H:%M:%S") != 0) {
                arena_maybe_free(buf);
                return 1;
            }
            break;
        case 'T':
            if (append_time_format(&buf, &len, &cap, "%I:%M:%S") != 0) {
                arena_maybe_free(buf);
                return 1;
            }
            break;
        case 'A':
            if (append_time_format(&buf, &len, &cap, "%H:%M") != 0) {
                arena_maybe_free(buf);
                return 1;
            }
            break;
        case '$':
            if (append_char(&buf, &len, &cap, geteuid() == 0 ? '#' : '$') != 0) {
                arena_maybe_free(buf);
                return 1;
            }
            break;
        case '#':
            if (append_unsigned_value(&buf, &len, &cap,
                                      (unsigned long)state->prompt_command_index) !=
                0) {
                arena_maybe_free(buf);
                return 1;
            }
            break;
        case 'j':
            if (append_unsigned_value(&buf, &len, &cap,
                                      (unsigned long)jobs_count_active()) != 0) {
                arena_maybe_free(buf);
                return 1;
            }
            break;
        default:
            if (append_char(&buf, &len, &cap, in[i]) != 0) {
                arena_maybe_free(buf);
                return 1;
            }
            break;
        }
    }

    if (buf == NULL) {
        buf = arena_xstrdup("");
    }
    *out = buf;
    return 0;
}

int prompt_render(struct shell_state *state, const char *var_name, char **out) {
    const char *raw;
    char *params;
    int rc;

    if (out == NULL || state == NULL || var_name == NULL) {
        return 1;
    }

    raw = getenv(var_name);
    if (raw == NULL) {
        *out = arena_xstrdup("");
        return 0;
    }

    params = NULL;
    rc = prompt_expand_simple_params(raw, state, &params);
    if (rc != 0) {
        return rc;
    }
    rc = prompt_expand_backslash_escapes(params, state, out);
    arena_maybe_free(params);
    return rc;
}

void prompt_init_defaults(struct shell_state *state, const char *argv0) {
    const char *base;

    if (state == NULL) {
        return;
    }

    base = basename_cstr(argv0);
    state->shell_name = arena_strdup_in(&state->arena_perm, base);

    if (!state->interactive) {
        return;
    }

    if (getenv("PS1") == NULL) {
        (void)vars_set_with_mode(state, "PS1", "\\w \\$ ", false, false);
    }
    if (getenv("PS2") == NULL) {
        (void)vars_set_with_mode(state, "PS2", "> ", false, false);
    }
}
