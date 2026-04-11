/* Jim Tcl public configuration for ESP32 (ESP-IDF) builds.
 *
 * On a normal host build, configure generates this from jim-config.h.in.
 * For cross-compilation we supply the values directly.
 */

#ifndef JIM_CONFIG_H
#define JIM_CONFIG_H

/* ESP32 toolchain (Xtensa / RISC-V) has long long */
#define HAVE_LONG_LONG 1

/* UTF-8 support disabled on ESP32 — the upstream build generates
 * _unicode_mapping.c at configure time, which is unavailable in
 * the cross-compile environment.  Basic ASCII works fine. */
/* #undef JIM_UTF8 */

/* int is 32 bits on ESP32 */
#define SIZEOF_INT 4

/* Jim version (0.84) */
#define JIM_VERSION 84

#endif /* JIM_CONFIG_H */
