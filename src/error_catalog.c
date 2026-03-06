/* SPDX-License-Identifier: 0BSD */

/* posish - centralized error catalog */

#include "error_catalog.h"

#include <stddef.h>

static const struct posish_error_def error_defs[] = {
    { POSERR_UNTERMINATED_COMMAND_SUBSTITUTION, POSISH_ERROR_KIND_SYNTAX,
      "unterminated command substitution" },
    { POSERR_UNTERMINATED_BACKTICK_SUBSTITUTION, POSISH_ERROR_KIND_SYNTAX,
      "unterminated backtick command substitution" },
    { POSERR_UNTERMINATED_DOLLAR_SINGLE_QUOTE, POSISH_ERROR_KIND_SYNTAX,
      "unterminated dollar-single-quoted string" },
    { POSERR_UNTERMINATED_PARAMETER_EXPANSION, POSISH_ERROR_KIND_SYNTAX,
      "unterminated parameter expansion" },
    { POSERR_TRAILING_BACKSLASH, POSISH_ERROR_KIND_SYNTAX,
      "trailing backslash in command" },
    { POSERR_UNTERMINATED_QUOTE, POSISH_ERROR_KIND_SYNTAX,
      "unterminated quote in command" },
    { POSERR_UNEXPECTED_EOF_MATCHING_QUOTE, POSISH_ERROR_KIND_SYNTAX,
      "unexpected EOF while looking for matching quote" },
    { POSERR_UNEXPECTED_TOKEN, POSISH_ERROR_KIND_SYNTAX,
      "syntax error near unexpected token `%s'" },
    { POSERR_MISSING_REDIRECTION_OPERAND, POSISH_ERROR_KIND_SYNTAX,
      "missing redirection operand" },
    { POSERR_AMBIGUOUS_REDIRECTION, POSISH_ERROR_KIND_RUNTIME,
      "ambiguous redirection" },
    { POSERR_INVALID_FD_REDIRECTION, POSISH_ERROR_KIND_RUNTIME,
      "invalid file descriptor redirection: %s" },
    { POSERR_UNSUPPORTED_TOKENS_AFTER_GROUP, POSISH_ERROR_KIND_SYNTAX,
      "unsupported tokens after grouped command" },
    { POSERR_COMPLEX_SYNTAX_UNIMPLEMENTED, POSISH_ERROR_KIND_SYNTAX,
      "complex shell syntax is not implemented yet" },

    { POSERR_CD_INVALID_OPTION, POSISH_ERROR_KIND_BUILTIN,
      "cd: invalid option: -%c" },
    { POSERR_CD_TOO_MANY_ARGUMENTS, POSISH_ERROR_KIND_BUILTIN,
      "cd: too many arguments" },
    { POSERR_CD_HOME_NOT_SET, POSISH_ERROR_KIND_BUILTIN,
      "cd: HOME is not set" },
    { POSERR_CD_OLDPWD_NOT_SET, POSISH_ERROR_KIND_BUILTIN,
      "cd: OLDPWD is not set" },

    { POSERR_PWD_INVALID_OPTION, POSISH_ERROR_KIND_BUILTIN,
      "pwd: invalid option: -%c" },
    { POSERR_PWD_TOO_MANY_ARGUMENTS, POSISH_ERROR_KIND_BUILTIN,
      "pwd: too many arguments" },

    { POSERR_UMASK_INVALID_OPTION, POSISH_ERROR_KIND_BUILTIN,
      "umask: invalid option: %s" },
    { POSERR_UMASK_TOO_MANY_OPERANDS, POSISH_ERROR_KIND_BUILTIN,
      "umask: too many operands" },
    { POSERR_UMASK_INVALID_MODE, POSISH_ERROR_KIND_BUILTIN,
      "umask: invalid mode: %s" },

    { POSERR_PRINTF_EXPECTED_NUMERIC_VALUE, POSISH_ERROR_KIND_BUILTIN,
      "printf: expected numeric value: %s" },
    { POSERR_PRINTF_MISSING_FORMAT_CHARACTER, POSISH_ERROR_KIND_BUILTIN,
      "printf: missing format character" },
    { POSERR_PRINTF_UNSUPPORTED_CONVERSION, POSISH_ERROR_KIND_BUILTIN,
      "printf: unsupported conversion: %%%c" },
    { POSERR_PRINTF_MISSING_FORMAT_OPERAND, POSISH_ERROR_KIND_BUILTIN,
      "printf: missing format operand" },

    { POSERR_KILL_AMBIGUOUS_JOB, POSISH_ERROR_KIND_BUILTIN,
      "kill: ambiguous job: %s" },
    { POSERR_KILL_INVALID_JOB_SPEC, POSISH_ERROR_KIND_BUILTIN,
      "kill: invalid job spec: %s" },
    { POSERR_KILL_NO_SUCH_JOB, POSISH_ERROR_KIND_BUILTIN,
      "kill: no such job: %s" },

    { POSERR_ALIAS_INVALID_NAME, POSISH_ERROR_KIND_BUILTIN,
      "alias: invalid name: %s" },
    { POSERR_ALIAS_NOT_FOUND, POSISH_ERROR_KIND_BUILTIN,
      "alias: %s: not found" },
    { POSERR_UNALIAS_INVALID_OPTION, POSISH_ERROR_KIND_BUILTIN,
      "unalias: invalid option: %s" },
    { POSERR_UNALIAS_MISSING_OPERAND, POSISH_ERROR_KIND_BUILTIN,
      "unalias: missing operand" },
    { POSERR_UNALIAS_INVALID_NAME, POSISH_ERROR_KIND_BUILTIN,
      "unalias: invalid name: %s" },
    { POSERR_UNALIAS_NOT_FOUND, POSISH_ERROR_KIND_BUILTIN,
      "unalias: %s: not found" },
    { POSERR_GETOPTS_MISSING_OPERANDS, POSISH_ERROR_KIND_BUILTIN,
      "getopts: missing operands" },
    { POSERR_GETOPTS_INVALID_VARIABLE_NAME, POSISH_ERROR_KIND_BUILTIN,
      "getopts: invalid variable name: %s" },
    { POSERR_GETOPTS_ILLEGAL_OPTION, POSISH_ERROR_KIND_BUILTIN,
      "getopts: illegal option -- %c" },
    { POSERR_GETOPTS_OPTION_REQUIRES_ARGUMENT, POSISH_ERROR_KIND_BUILTIN,
      "getopts: option requires an argument -- %c" },
    { POSERR_HASH_INVALID_OPTION, POSISH_ERROR_KIND_BUILTIN,
      "hash: invalid option: %s" },
    { POSERR_TYPE_MISSING_OPERAND, POSISH_ERROR_KIND_BUILTIN,
      "type: missing operand" },
    { POSERR_JOBSPEC_AMBIGUOUS, POSISH_ERROR_KIND_BUILTIN,
      "%s: ambiguous job: %s" },
    { POSERR_JOBSPEC_INVALID, POSISH_ERROR_KIND_BUILTIN,
      "%s: invalid job spec: %s" },
    { POSERR_JOBSPEC_NO_SUCH, POSISH_ERROR_KIND_BUILTIN,
      "%s: no such job: %s" },
    { POSERR_WAIT_UNSUPPORTED_OPERAND, POSISH_ERROR_KIND_BUILTIN,
      "wait: unsupported operand: %s" },
    { POSERR_FG_JOB_CONTROL_DISABLED, POSISH_ERROR_KIND_BUILTIN,
      "fg: job control is disabled" },
    { POSERR_FG_TOO_MANY_ARGUMENTS, POSISH_ERROR_KIND_BUILTIN,
      "fg: too many arguments" },
    { POSERR_FG_NO_CURRENT_JOB, POSISH_ERROR_KIND_BUILTIN,
      "fg: no current job" },
    { POSERR_BG_JOB_CONTROL_DISABLED, POSISH_ERROR_KIND_BUILTIN,
      "bg: job control is disabled" },
    { POSERR_BG_NO_CURRENT_JOB, POSISH_ERROR_KIND_BUILTIN,
      "bg: no current job" },

    { POSERR_COMMAND_INVALID_OPTION, POSISH_ERROR_KIND_BUILTIN,
      "command: invalid option: -%c" },
    { POSERR_SHIFT_INVALID_COUNT, POSISH_ERROR_KIND_BUILTIN,
      "shift: invalid shift count: %s" },
    { POSERR_SHIFT_TOO_MANY_ARGUMENTS, POSISH_ERROR_KIND_BUILTIN,
      "shift: too many arguments" },
    { POSERR_SHIFT_COUNT_OUT_OF_RANGE, POSISH_ERROR_KIND_BUILTIN,
      "shift: shift count out of range" },
    { POSERR_SET_INVALID_OPTION_NAME, POSISH_ERROR_KIND_BUILTIN,
      "set: invalid option name: %s" },
    { POSERR_SET_INVALID_OPTION, POSISH_ERROR_KIND_BUILTIN,
      "set: invalid option: -%c" },
    { POSERR_LOOP_INVALID_COUNT, POSISH_ERROR_KIND_BUILTIN,
      "%s: invalid loop count: %s" },
    { POSERR_LOOP_TOO_MANY_ARGUMENTS, POSISH_ERROR_KIND_BUILTIN,
      "%s: too many arguments" },
    { POSERR_LOOP_ONLY_IN_LOOP, POSISH_ERROR_KIND_BUILTIN,
      "%s: only meaningful in a loop" },
    { POSERR_RETURN_NUMERIC_ARGUMENT_REQUIRED, POSISH_ERROR_KIND_BUILTIN,
      "return: numeric argument required: %s" },
    { POSERR_RETURN_TOO_MANY_ARGUMENTS, POSISH_ERROR_KIND_BUILTIN,
      "return: too many arguments" },
    { POSERR_RETURN_OUTSIDE_CONTEXT, POSISH_ERROR_KIND_BUILTIN,
      "return: can only be used in a function or sourced script" },
    { POSERR_DOT_FILENAME_ARGUMENT_REQUIRED, POSISH_ERROR_KIND_BUILTIN,
      ".: filename argument required" },
    { POSERR_DOT_FILE_NOT_FOUND, POSISH_ERROR_KIND_BUILTIN,
      ".: %s: file not found" },
    { POSERR_UNSET_INVALID_OPTION, POSISH_ERROR_KIND_BUILTIN,
      "unset: invalid option: -%c" },
    { POSERR_UNSET_INVALID_FUNCTION_NAME, POSISH_ERROR_KIND_BUILTIN,
      "unset: invalid function name: %s" },
    { POSERR_EXPORT_INVALID_OPTION, POSISH_ERROR_KIND_BUILTIN,
      "export: invalid option: -%c" },
    { POSERR_EXPORT_INVALID_VARIABLE_NAME, POSISH_ERROR_KIND_BUILTIN,
      "export: invalid variable name: %s" },
    { POSERR_READ_OPTION_REQUIRES_ARGUMENT, POSISH_ERROR_KIND_BUILTIN,
      "read: option -d requires an argument" },
    { POSERR_READ_INVALID_OPTION, POSISH_ERROR_KIND_BUILTIN,
      "read: invalid option: -%c" },
    { POSERR_READONLY_INVALID_OPTION, POSISH_ERROR_KIND_BUILTIN,
      "readonly: invalid option: -%c" },
    { POSERR_TRAP_INVALID_SIGNAL, POSISH_ERROR_KIND_BUILTIN,
      "trap: invalid signal: %s" },
    { POSERR_TRAP_MISSING_CONDITION, POSISH_ERROR_KIND_BUILTIN,
      "trap: missing condition" },
    { POSERR_EXIT_NUMERIC_ARGUMENT_REQUIRED, POSISH_ERROR_KIND_BUILTIN,
      "exit: numeric argument required: %s" },
    { POSERR_EXIT_TOO_MANY_ARGUMENTS, POSISH_ERROR_KIND_BUILTIN,
      "exit: too many arguments" }
};

const struct posish_error_def *posish_error_lookup(enum posish_error_id id) {
    size_t i;

    for (i = 0; i < sizeof(error_defs) / sizeof(error_defs[0]); i++) {
        if (error_defs[i].id == id) {
            return &error_defs[i];
        }
    }
    return NULL;
}

const char *posish_error_kind_name(enum posish_error_kind kind) {
    switch (kind) {
    case POSISH_ERROR_KIND_SYNTAX:
        return "syntax";
    case POSISH_ERROR_KIND_BUILTIN:
        return "builtin";
    case POSISH_ERROR_KIND_RUNTIME:
        return "runtime";
    case POSISH_ERROR_KIND_SYSTEM:
        return "system";
    default:
        return "error";
    }
}
