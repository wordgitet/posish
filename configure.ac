AC_PREREQ([2.69])
AC_INIT([posish], [0.1.3], [https://github.com/example/posish])
AC_CONFIG_SRCDIR([src/main.c])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_AUX_DIR([build-aux])

AC_CANONICAL_BUILD
AC_CANONICAL_HOST
AC_CANONICAL_TARGET

AC_PROG_CC
AC_USE_SYSTEM_EXTENSIONS
AC_SYS_LARGEFILE

AC_CHECK_HEADERS([fcntl.h pty.h util.h termios.h spawn.h sys/select.h sys/wait.h])
AC_CHECK_FUNCS([fork execve posix_spawn posix_spawnp setpgid tcsetpgrp tcgetpgrp \
                sigaction sigprocmask waitpid getline select])

AC_ARG_ENABLE([trace],
    [AS_HELP_STRING([--enable-trace], [enable runtime tracing hooks (default: no)])],
    [enable_trace="$enableval"],
    [enable_trace="no"])
AS_IF([test "x$enable_trace" = "xyes"], [
    AC_DEFINE([POSISH_ENABLE_TRACE], [1],
              [Define to 1 to enable runtime tracing hooks.])
], [
    AC_DEFINE([POSISH_ENABLE_TRACE], [0],
              [Define to 1 to enable runtime tracing hooks.])
])

AC_PATH_PROG([YASH_RUNNER], [yash], [])
AS_IF([test -z "$YASH_RUNNER"], [
    AC_MSG_WARN([system yash not found in PATH; test targets will fail clearly until YASH_RUNNER is set])
])
AC_SUBST([YASH_RUNNER])

AC_CONFIG_FILES([
    Makefile
    tests/posix/Makefile
    config.mk
])
AC_OUTPUT
