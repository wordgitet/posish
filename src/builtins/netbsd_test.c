/* SPDX-License-Identifier: 0BSD */

/* posish - netbsd test builtin wrapper */

#include "builtins/netbsd_test.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

/* Compile the vendored NetBSD test(1) source in shell-builtin mode. */
#define SHELL 1

/* NetBSD-local helper macros that may be absent on non-NetBSD libc headers. */
#ifndef __RCSID
#define __RCSID(x) \
    static const char netbsd_test_rcsid[] __attribute__((unused)) = x
#endif
#ifndef __dead
#define __dead __attribute__((__noreturn__))
#endif
#ifndef __printflike
#define __printflike(a, b) __attribute__((__format__(__printf__, a, b)))
#endif
#ifndef __arraycount
#define __arraycount(a) (sizeof(a) / sizeof((a)[0]))
#endif
#ifndef timespeccmp
#define timespeccmp(tsp, usp, cmp) \
    (((tsp)->tv_sec == (usp)->tv_sec) ? \
         ((tsp)->tv_nsec cmp(usp)->tv_nsec) : \
         ((tsp)->tv_sec cmp(usp)->tv_sec))
#endif

/*
 * NetBSD test(1) reports syntax/runtime issues via error() and exits with status 2.
 * For builtin usage we map that into a non-fatal longjmp return path.
 */
static jmp_buf g_test_error_jmp;
static int g_test_error_active;

void netbsd_test_error(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    fprintf(stderr, "test: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);

    if (g_test_error_active) {
        longjmp(g_test_error_jmp, 1);
    }
    exit(2);
}

void *netbsd_test_ckmalloc(size_t size)
{
    void *ptr;

    ptr = malloc(size);
    if (ptr == NULL) {
        netbsd_test_error("Not enough memory!");
    }
    return ptr;
}

#define error netbsd_test_error
#define ckmalloc netbsd_test_ckmalloc

#include "builtins/netbsd_test_vendor.c"

#undef error
#undef ckmalloc

int posish_netbsd_test_builtin(int argc, char *const argv[])
{
    if (setjmp(g_test_error_jmp) != 0) {
        g_test_error_active = 0;
        return 2;
    }

    g_test_error_active = 1;
    /* testcmd() comes from the vendored NetBSD test(1) source. */
    argc = testcmd(argc, (char **)argv);
    g_test_error_active = 0;
    return argc;
}
