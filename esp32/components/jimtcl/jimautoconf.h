/* Jim Tcl autoconf stub for ESP32 (ESP-IDF) builds.
 *
 * On a normal host build, configure generates this header.
 * For cross-compilation under ESP-IDF we provide the minimal
 * set of defines that the core sources expect.
 */

#ifndef JIMAUTOCONF_H
#define JIMAUTOCONF_H

/* We use the ESP-IDF heap; no mmap/sbrk */
/* #undef HAVE_SYS_SYSINFO_H */
/* #undef HAVE_SYS_SIGLIST */

/* Jim extensions compiled into the ESP32 build */
#define jim_ext_package 1
#define jim_ext_array 1
#define jim_ext_clock 1
#define jim_ext_interp 1
#define jim_ext_namespace 1
#define jim_ext_tclprefix 1

/* Platform identification */
#define TCL_PLATFORM_OS "esp-idf"
#define TCL_PLATFORM_PLATFORM "esp32"
#define TCL_PLATFORM_PATH_SEPARATOR ":"

/* No dynamic loading on ESP32 */
/* #undef JIM_DYNLIB */

/* No signal support */
/* #undef JIM_SIGNAL */

/* No exec/fork on bare-metal */
/* #undef HAVE_EXECVPE */
/* #undef HAVE_FORK */
/* #undef HAVE_VFORK */
/* #undef HAVE_WAITPID */
/* #undef HAVE_PIPE */

/* String/POSIX functions available via newlib */
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_UNISTD_H 1
#define HAVE_SYS_TIME_H 1

/* No real filesystem features */
/* #undef HAVE_REALPATH */
/* #undef HAVE_SYMLINK */
/* #undef HAVE_READLINK */
/* #undef HAVE_UTIMES */
/* #undef HAVE_REGCOMP */

#endif /* JIMAUTOCONF_H */
