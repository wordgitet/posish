/* SPDX-License-Identifier: 0BSD */

/*
 * posish - portability shims used across the codebase
 *
 * This header centralizes fallback definitions for BSD/NetBSD-style macros
 * and platform gaps hit by both runtime code and imported vendor code.
 *
 * Note: runtime code relies on POSIX.1-2008 interfaces (getline, setenv,
 * unsetenv, mkstemp). Those are expected from the host libc/toolchain and are
 * not reimplemented here.
 */

#ifndef POSISH_COMPAT_H
#define POSISH_COMPAT_H

#include <sys/stat.h>
#include <sys/types.h>

/* -----------------------------------------------------------------------
 * Compiler attribute macros
 * ----------------------------------------------------------------------- */

/* __dead / __noreturn — function never returns */
#ifndef __dead
# if defined(__GNUC__) || defined(__clang__)
#  define __dead __attribute__((__noreturn__))
# elif defined(_MSC_VER)
#  define __dead __declspec(noreturn)
# elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#  define __dead _Noreturn
# else
#  define __dead
# endif
#endif

/* __printflike — printf-format checking */
#ifndef __printflike
# if defined(__GNUC__) || defined(__clang__)
#  define __printflike(fmtarg, firstvararg) \
     __attribute__((__format__(__printf__, fmtarg, firstvararg)))
# else
#  define __printflike(fmtarg, firstvararg)
# endif
#endif

/* __scanflike — scanf-format checking */
#ifndef __scanflike
# if defined(__GNUC__) || defined(__clang__)
#  define __scanflike(fmtarg, firstvararg) \
     __attribute__((__format__(__scanf__, fmtarg, firstvararg)))
# else
#  define __scanflike(fmtarg, firstvararg)
# endif
#endif

/* __unused — suppress unused-variable warnings */
#ifndef __unused
# if defined(__GNUC__) || defined(__clang__)
#  define __unused __attribute__((__unused__))
# else
#  define __unused
# endif
#endif

/* __pure — function has no side effects */
#ifndef __pure
# if defined(__GNUC__) || defined(__clang__)
#  define __pure __attribute__((__pure__))
# else
#  define __pure
# endif
#endif

/* __nonnull — pointer arguments must not be NULL */
#ifndef __nonnull
# if defined(__GNUC__) || defined(__clang__)
#  define __nonnull(args) __attribute__((__nonnull__ args))
# else
#  define __nonnull(args)
# endif
#endif

/* __packed — disable struct padding */
#ifndef __packed
# if defined(__GNUC__) || defined(__clang__)
#  define __packed __attribute__((__packed__))
# elif defined(_MSC_VER)
#  define __packed
# else
#  define __packed
# endif
#endif

/* __aligned — alignment hint */
#ifndef __aligned
# if defined(__GNUC__) || defined(__clang__)
#  define __aligned(n) __attribute__((__aligned__(n)))
# else
#  define __aligned(n)
# endif
#endif

/* __predict_true / __predict_false — branch prediction hints */
#ifndef __predict_true
# if defined(__GNUC__) || defined(__clang__)
#  define __predict_true(exp)  __builtin_expect(!!(exp), 1)
#  define __predict_false(exp) __builtin_expect(!!(exp), 0)
# else
#  define __predict_true(exp)  (exp)
#  define __predict_false(exp) (exp)
# endif
#endif

/* -----------------------------------------------------------------------
 * NetBSD / BSD source annotation macros
 * ----------------------------------------------------------------------- */

/* __RCSID — embed RCS version string (no-op outside BSD) */
#ifndef __RCSID
# define __RCSID(x)
#endif

/* __COPYRIGHT — embed copyright string (no-op outside BSD) */
#ifndef __COPYRIGHT
# define __COPYRIGHT(x)
#endif

/* __KERNEL_RCSID — kernel RCS id (no-op outside NetBSD kernel) */
#ifndef __KERNEL_RCSID
# define __KERNEL_RCSID(n, x)
#endif

/* -----------------------------------------------------------------------
 * Array / size helpers
 * ----------------------------------------------------------------------- */

/* __arraycount — number of elements in a statically-sized array */
#ifndef __arraycount
# define __arraycount(a) (sizeof(a) / sizeof((a)[0]))
#endif

/* nitems — alias used on OpenBSD/FreeBSD */
#ifndef nitems
# define nitems(a) (sizeof(a) / sizeof((a)[0]))
#endif

/* -----------------------------------------------------------------------
 * Timespec comparison
 *
 * timespeccmp(tsp, usp, cmp):
 *   NetBSD/FreeBSD/OpenBSD macro that compares two struct timespec values
 *   using operator cmp (==, !=, <, <=, >, >=).
 *
 *   Available natively on:  NetBSD, FreeBSD, OpenBSD, DragonFlyBSD
 *   NOT available on:       Linux (glibc/musl), macOS, Solaris, Haiku
 * ----------------------------------------------------------------------- */
#ifndef timespeccmp
# define timespeccmp(tsp, usp, cmp) \
    (((tsp)->tv_sec == (usp)->tv_sec) \
        ? ((tsp)->tv_nsec cmp (usp)->tv_nsec) \
        : ((tsp)->tv_sec  cmp (usp)->tv_sec))
#endif

/* timespecadd / timespecsub — also missing on some platforms */
#ifndef timespecadd
# define timespecadd(tsp, usp, vsp) \
    do { \
        (vsp)->tv_sec  = (tsp)->tv_sec  + (usp)->tv_sec;  \
        (vsp)->tv_nsec = (tsp)->tv_nsec + (usp)->tv_nsec; \
        if ((vsp)->tv_nsec >= 1000000000L) { \
            (vsp)->tv_sec++;                \
            (vsp)->tv_nsec -= 1000000000L;  \
        }                                   \
    } while (0)
#endif

#ifndef timespecsub
# define timespecsub(tsp, usp, vsp) \
    do { \
        (vsp)->tv_sec  = (tsp)->tv_sec  - (usp)->tv_sec;  \
        (vsp)->tv_nsec = (tsp)->tv_nsec - (usp)->tv_nsec; \
        if ((vsp)->tv_nsec < 0) { \
            (vsp)->tv_sec--;              \
            (vsp)->tv_nsec += 1000000000L;\
        }                                 \
    } while (0)
#endif

/*
 * stat(2) timestamp member compatibility:
 * - Linux/BSD commonly expose st_[amc]tim.
 * - macOS exposes st_[amc]timespec.
 */
#if defined(__APPLE__) && defined(__MACH__)
# ifndef st_atim
#  define st_atim st_atimespec
# endif
# ifndef st_mtim
#  define st_mtim st_mtimespec
# endif
# ifndef st_ctim
#  define st_ctim st_ctimespec
# endif
#endif

/* -----------------------------------------------------------------------
 * NGROUPS_MAX — maximum number of supplementary groups
 *
 * POSIX requires this but some minimal environments omit it.
 * ----------------------------------------------------------------------- */
#ifndef NGROUPS_MAX
# ifdef _SC_NGROUPS_MAX
#  include <unistd.h>
   /* Will be queried at runtime via sysconf() where needed */
#  define NGROUPS_MAX 65536
# else
#  define NGROUPS_MAX 16   /* conservative POSIX minimum */
# endif
#endif

/* -----------------------------------------------------------------------
 * MIN / MAX — safe versions
 * ----------------------------------------------------------------------- */
#ifndef MIN
# define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
# define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

/* -----------------------------------------------------------------------
 * Haiku compatibility
 * ----------------------------------------------------------------------- */
#ifdef __HAIKU__
# include <OS.h>      /* for various Haiku types */
# ifndef S_ISVTX
#  define S_ISVTX 0001000
# endif
#endif

/* -----------------------------------------------------------------------
 * Solaris / illumos compatibility
 * ----------------------------------------------------------------------- */
#if defined(__sun) || defined(__svr4__)
# ifndef S_ISVTX
#  define S_ISVTX 0001000
# endif
#endif

/* -----------------------------------------------------------------------
 * Android / Bionic (Termux) compatibility
 * ----------------------------------------------------------------------- */
#ifdef __ANDROID__
# ifndef NGROUPS_MAX
#  define NGROUPS_MAX 65536
# endif
#endif

#endif /* POSISH_COMPAT_H */
