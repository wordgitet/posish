/* SPDX-License-Identifier: 0BSD */

/* posish - options subsystem */

#include "options.h"

#include <string.h>

struct option_spec {
    char short_name;
    const char *long_name;
};

/*
 * Keep option ordering deterministic for `$-`, `set -o`, and `set +o`.
 * The exact order is not mandated by POSIX, but tests expect stable output.
 */
static const struct option_spec k_option_specs[] = {
    {'a', "allexport"}, {'b', "notify"},  {'C', "noclobber"},
    {'e', "errexit"},   {'f', "noglob"},  {'h', "hashondef"},
    {'i', "interactive"}, {'m', "monitor"},
    {'n', "noexec"},    {'u', "nounset"}, {'v', "verbose"},
    {'x', "xtrace"},    {'\0', "pipefail"}, {'\0', "ignoreeof"}};

static bool *option_slot(struct shell_state *state, char short_name,
                         const char *long_name, bool *is_signal_policy_option) {
    if (is_signal_policy_option != NULL) {
        *is_signal_policy_option = false;
    }

    if (short_name == 'a' || (long_name != NULL && strcmp(long_name, "allexport") == 0)) {
        return &state->allexport;
    }
    if (short_name == 'b' || (long_name != NULL && strcmp(long_name, "notify") == 0)) {
        return &state->notify;
    }
    if (short_name == 'C' || (long_name != NULL && strcmp(long_name, "noclobber") == 0)) {
        return &state->noclobber;
    }
    if (short_name == 'e' || (long_name != NULL && strcmp(long_name, "errexit") == 0)) {
        return &state->errexit;
    }
    if (short_name == 'f' || (long_name != NULL && strcmp(long_name, "noglob") == 0)) {
        return &state->noglob;
    }
    if (short_name == 'h' || (long_name != NULL && strcmp(long_name, "hashondef") == 0)) {
        return &state->hashondef;
    }
    if (short_name == 'i' || (long_name != NULL && strcmp(long_name, "interactive") == 0)) {
        if (is_signal_policy_option != NULL) {
            *is_signal_policy_option = true;
        }
        return &state->interactive;
    }
    if (short_name == 'm' || (long_name != NULL && strcmp(long_name, "monitor") == 0)) {
        if (is_signal_policy_option != NULL) {
            *is_signal_policy_option = true;
        }
        return &state->monitor_mode;
    }
    if (short_name == 'n' || (long_name != NULL && strcmp(long_name, "noexec") == 0)) {
        return &state->noexec;
    }
    if (short_name == 'u' || (long_name != NULL && strcmp(long_name, "nounset") == 0)) {
        return &state->nounset;
    }
    if (short_name == 'v' || (long_name != NULL && strcmp(long_name, "verbose") == 0)) {
        return &state->verbose;
    }
    if (short_name == 'x' || (long_name != NULL && strcmp(long_name, "xtrace") == 0)) {
        return &state->xtrace;
    }
    if (long_name != NULL && strcmp(long_name, "pipefail") == 0) {
        return &state->pipefail;
    }
    if (long_name != NULL && strcmp(long_name, "ignoreeof") == 0) {
        return &state->ignoreeof;
    }

    return NULL;
}

static bool option_enabled_by_short(const struct shell_state *state, char short_name) {
    if (short_name == 'a') {
        return state->allexport;
    }
    if (short_name == 'b') {
        return state->notify;
    }
    if (short_name == 'C') {
        return state->noclobber;
    }
    if (short_name == 'e') {
        return state->errexit;
    }
    if (short_name == 'f') {
        return state->noglob;
    }
    if (short_name == 'h') {
        return state->hashondef;
    }
    if (short_name == 'i') {
        return state->interactive;
    }
    if (short_name == 'm') {
        return state->monitor_mode;
    }
    if (short_name == 'n') {
        return state->noexec;
    }
    if (short_name == 'u') {
        return state->nounset;
    }
    if (short_name == 'v') {
        return state->verbose;
    }
    if (short_name == 'x') {
        return state->xtrace;
    }
    return false;
}

static bool option_enabled_by_long(const struct shell_state *state,
                                   const char *long_name) {
    if (strcmp(long_name, "pipefail") == 0) {
        return state->pipefail;
    }
    if (strcmp(long_name, "ignoreeof") == 0) {
        return state->ignoreeof;
    }
    return false;
}

void options_init(void) {
    /* Placeholder for shell options (set -o, shopt-like state, etc.). */
}

bool options_apply_short(struct shell_state *state, char short_name, bool enable,
                         bool *refresh_signal_policy) {
    bool signal_policy_option;
    bool *slot;
    bool old_value;

    slot = option_slot(state, short_name, NULL, &signal_policy_option);
    if (slot == NULL) {
        return false;
    }

    old_value = *slot;
    *slot = enable;
    if (refresh_signal_policy != NULL && signal_policy_option &&
        old_value != enable) {
        *refresh_signal_policy = true;
    }
    return true;
}

bool options_apply_long(struct shell_state *state, const char *long_name,
                        bool enable, bool *refresh_signal_policy) {
    bool signal_policy_option;
    bool *slot;
    bool old_value;

    if (long_name == NULL || long_name[0] == '\0') {
        return false;
    }

    slot = option_slot(state, '\0', long_name, &signal_policy_option);
    if (slot == NULL) {
        return false;
    }

    old_value = *slot;
    *slot = enable;
    if (refresh_signal_policy != NULL && signal_policy_option &&
        old_value != enable) {
        *refresh_signal_policy = true;
    }
    return true;
}

void options_format_dollar_minus(const struct shell_state *state, char *out,
                                 size_t out_size) {
    size_t i;
    size_t pos;

    if (out_size == 0) {
        return;
    }

    pos = 0;
    for (i = 0; i < sizeof(k_option_specs) / sizeof(k_option_specs[0]); i++) {
        char short_name;

        short_name = k_option_specs[i].short_name;
        if (short_name == '\0') {
            continue;
        }
        if (!option_enabled_by_short(state, short_name)) {
            continue;
        }
        if (pos + 1 >= out_size) {
            break;
        }
        out[pos++] = short_name;
    }
    out[pos] = '\0';
}

int options_print_set_o(FILE *out, const struct shell_state *state) {
    size_t i;

    for (i = 0; i < sizeof(k_option_specs) / sizeof(k_option_specs[0]); i++) {
        const struct option_spec *spec;
        bool enabled;

        spec = &k_option_specs[i];
        if (spec->short_name != '\0') {
            enabled = option_enabled_by_short(state, spec->short_name);
        } else {
            enabled = option_enabled_by_long(state, spec->long_name);
        }
        if (fprintf(out, "%s\t%s\n", spec->long_name, enabled ? "on" : "off") < 0) {
            return 1;
        }
    }
    return 0;
}

int options_print_set_plus_o(FILE *out, const struct shell_state *state) {
    size_t i;

    for (i = 0; i < sizeof(k_option_specs) / sizeof(k_option_specs[0]); i++) {
        const struct option_spec *spec;
        bool enabled;

        spec = &k_option_specs[i];
        if (spec->short_name != '\0') {
            enabled = option_enabled_by_short(state, spec->short_name);
        } else {
            enabled = option_enabled_by_long(state, spec->long_name);
        }
        if (fprintf(out, "set %co %s\n", enabled ? '-' : '+', spec->long_name) <
            0) {
            return 1;
        }
    }
    return 0;
}
