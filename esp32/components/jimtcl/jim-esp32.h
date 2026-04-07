/* Jim Tcl ESP32 Platform Header
 *
 * Platform abstractions for running Jim Tcl on ESP32 with ESP-IDF / FreeRTOS.
 */

#ifndef JIM_ESP32_H
#define JIM_ESP32_H

#include <jim.h>
#include "soc/soc_caps.h"

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
#if defined(SOC_IEEE802154_SUPPORTED) && SOC_IEEE802154_SUPPORTED
int Jim_ieee802154Init(Jim_Interp *interp);
#endif
int Jim_sleepInit(Jim_Interp *interp);
int Jim_watchdogInit(Jim_Interp *interp);

/* Protocol extensions (Kconfig-gated) */
#ifdef CONFIG_JIM_EXT_HTTP
int Jim_httpInit(Jim_Interp *interp);
#endif
#ifdef CONFIG_JIM_EXT_MQTT
int Jim_mqttInit(Jim_Interp *interp);
#endif
#ifdef CONFIG_JIM_EXT_WEBSOCKET
int Jim_websocketInit(Jim_Interp *interp);
#endif
#ifdef CONFIG_JIM_EXT_JSON
int Jim_jsonInit(Jim_Interp *interp);
#endif
#if defined(CONFIG_JIM_EXT_TWAI) && defined(SOC_TWAI_SUPPORTED)
int Jim_twaiInit(Jim_Interp *interp);
#endif
#ifdef CONFIG_JIM_EXT_SERIAL
int Jim_serialInit(Jim_Interp *interp);
#endif
#ifdef CONFIG_JIM_EXT_ADC
int Jim_adcInit(Jim_Interp *interp);
#endif
#ifdef CONFIG_JIM_EXT_PWM
int Jim_pwmInit(Jim_Interp *interp);
#endif
#ifdef CONFIG_JIM_EXT_TIMER
int Jim_timerInit(Jim_Interp *interp);
#endif
#ifdef CONFIG_JIM_EXT_FS
int Jim_fsInit(Jim_Interp *interp);
#endif
#ifdef CONFIG_JIM_EXT_OTA
int Jim_otaInit(Jim_Interp *interp);
#endif
#ifdef CONFIG_JIM_EXT_SPI
int Jim_spiInit(Jim_Interp *interp);
#endif
#ifdef CONFIG_JIM_EXT_ONEWIRE
int Jim_onewireInit(Jim_Interp *interp);
#endif
#ifdef CONFIG_JIM_EXT_CRON
int Jim_cronInit(Jim_Interp *interp);
#endif
#ifdef CONFIG_JIM_EXT_SYSLOG
int Jim_syslogInit(Jim_Interp *interp);
#endif
#ifdef CONFIG_JIM_EXT_TEST
int Jim_testInit(Jim_Interp *interp);
#endif

#ifdef __cplusplus
}
#endif

#endif /* JIM_ESP32_H */
