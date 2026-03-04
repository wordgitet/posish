#ifndef POSISH_VERSION_H
#define POSISH_VERSION_H

/*
 * Single source of truth: configure.ac AC_INIT version -> PACKAGE_VERSION.
 * Build system injects config.h, which defines PACKAGE_VERSION.
 */
#ifdef PACKAGE_VERSION
#define POSISH_VERSION PACKAGE_VERSION
#else
#define POSISH_VERSION "unknown"
#endif

#endif
