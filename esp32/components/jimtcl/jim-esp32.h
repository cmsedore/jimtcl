/* Jim Tcl ESP32 Platform Header
 *
 * Platform abstractions for running Jim Tcl on ESP32 with ESP-IDF / FreeRTOS.
 */

#ifndef JIM_ESP32_H
#define JIM_ESP32_H

#include <jim.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize ESP32 platform support for a Jim interpreter.
 * Sets up tcl_platform variables, registers the ESP32-specific
 * extensions, and configures the allocator.
 *
 * Call this after Jim_CreateInterp() and Jim_RegisterCoreCommands().
 */
int Jim_Esp32PlatformInit(Jim_Interp *interp);

/**
 * Run an interactive Tcl REPL over UART.
 * Blocks the calling FreeRTOS task and reads/evaluates lines.
 */
int Jim_Esp32InteractivePrompt(Jim_Interp *interp);

/**
 * Initialize all static ESP32 extensions plus the standard Jim ones.
 * This replaces Jim_InitStaticExtensions() for the ESP32 build.
 */
int Jim_InitStaticExtensions(Jim_Interp *interp);

/* Extension init functions */
int Jim_gpioInit(Jim_Interp *interp);
int Jim_wifiInit(Jim_Interp *interp);
int Jim_i2cInit(Jim_Interp *interp);
int Jim_nvsInit(Jim_Interp *interp);
int Jim_esp_taskInit(Jim_Interp *interp);

#ifdef __cplusplus
}
#endif

#endif /* JIM_ESP32_H */
