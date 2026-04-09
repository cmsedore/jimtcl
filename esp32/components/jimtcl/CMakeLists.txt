# Jim Tcl ESP-IDF Component
#
# This builds the Jim Tcl interpreter core and ESP32-specific extensions
# as an ESP-IDF component.

set(JIMTCL_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")

# Core interpreter sources
set(JIMTCL_CORE_SRCS
    "${JIMTCL_ROOT}/jim.c"
    "${JIMTCL_ROOT}/jim-subcmd.c"
    "${JIMTCL_ROOT}/jim-interactive.c"
    "${JIMTCL_ROOT}/jim-format.c"
    "${JIMTCL_ROOT}/jim-package.c"
    "${JIMTCL_ROOT}/jim-namespace.c"
    "${JIMTCL_ROOT}/jim-interp.c"
    "${JIMTCL_ROOT}/jim-array.c"
    "${JIMTCL_ROOT}/jim-clock.c"
    "${JIMTCL_ROOT}/jim-tclprefix.c"
    "${JIMTCL_ROOT}/jim-nosignal.c"
    "${JIMTCL_ROOT}/jimregexp.c"
    "${JIMTCL_ROOT}/utf8.c"
)

# ESP32 platform shim and extensions
set(JIMTCL_ESP32_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/jim-esp32.c"
    "${CMAKE_CURRENT_LIST_DIR}/jim-esp32-loadext.c"
    "${CMAKE_CURRENT_LIST_DIR}/extensions/jim-gpio.c"
    "${CMAKE_CURRENT_LIST_DIR}/extensions/jim-wifi.c"
    "${CMAKE_CURRENT_LIST_DIR}/extensions/jim-i2c.c"
    "${CMAKE_CURRENT_LIST_DIR}/extensions/jim-nvs.c"
    "${CMAKE_CURRENT_LIST_DIR}/extensions/jim-esp-task.c"
    "${CMAKE_CURRENT_LIST_DIR}/extensions/jim-sleep.c"
    "${CMAKE_CURRENT_LIST_DIR}/extensions/jim-watchdog.c"
)

set(JIMTCL_REQUIRES
    driver
    esp_wifi
    esp_event
    nvs_flash
    esp_timer
    esp_hw_support
    vfs
    spiffs
    console
    esp_http_client
    mqtt
    json
    esp_adc
    esp_https_ota
    app_update
)

# IEEE 802.15.4 is only available on ESP32-C6 and ESP32-H2
if(IDF_TARGET STREQUAL "esp32c6" OR IDF_TARGET STREQUAL "esp32h2")
    list(APPEND JIMTCL_ESP32_SRCS
        "${CMAKE_CURRENT_LIST_DIR}/extensions/jim-ieee802154.c")
    list(APPEND JIMTCL_REQUIRES ieee802154)
endif()

# Kconfig-gated protocol extensions
if(CONFIG_JIM_EXT_HTTP)
    list(APPEND JIMTCL_ESP32_SRCS
        "${CMAKE_CURRENT_LIST_DIR}/extensions/jim-http.c")
    list(APPEND JIMTCL_REQUIRES esp_http_client)
endif()

if(CONFIG_JIM_EXT_MQTT)
    list(APPEND JIMTCL_ESP32_SRCS
        "${CMAKE_CURRENT_LIST_DIR}/extensions/jim-mqtt.c")
    list(APPEND JIMTCL_REQUIRES mqtt)
endif()

if(CONFIG_JIM_EXT_WEBSOCKET)
    list(APPEND JIMTCL_ESP32_SRCS
        "${CMAKE_CURRENT_LIST_DIR}/extensions/jim-websocket.c")
endif()

if(CONFIG_JIM_EXT_JSON)
    list(APPEND JIMTCL_ESP32_SRCS
        "${CMAKE_CURRENT_LIST_DIR}/extensions/jim-json.c")
endif()

if(CONFIG_JIM_EXT_TWAI)
    list(APPEND JIMTCL_ESP32_SRCS
        "${CMAKE_CURRENT_LIST_DIR}/extensions/jim-twai.c")
endif()

if(CONFIG_JIM_EXT_SERIAL)
    list(APPEND JIMTCL_ESP32_SRCS
        "${CMAKE_CURRENT_LIST_DIR}/extensions/jim-serial.c")
endif()

if(CONFIG_JIM_EXT_ADC)
    list(APPEND JIMTCL_ESP32_SRCS
        "${CMAKE_CURRENT_LIST_DIR}/extensions/jim-adc.c")
    list(APPEND JIMTCL_REQUIRES esp_adc)
endif()

if(CONFIG_JIM_EXT_PWM)
    list(APPEND JIMTCL_ESP32_SRCS
        "${CMAKE_CURRENT_LIST_DIR}/extensions/jim-pwm.c")
endif()

if(CONFIG_JIM_EXT_TIMER)
    list(APPEND JIMTCL_ESP32_SRCS
        "${CMAKE_CURRENT_LIST_DIR}/extensions/jim-timer.c")
endif()

if(CONFIG_JIM_EXT_FS)
    list(APPEND JIMTCL_ESP32_SRCS
        "${CMAKE_CURRENT_LIST_DIR}/extensions/jim-fs.c")
endif()

if(CONFIG_JIM_EXT_OTA)
    list(APPEND JIMTCL_ESP32_SRCS
        "${CMAKE_CURRENT_LIST_DIR}/extensions/jim-ota.c")
    list(APPEND JIMTCL_REQUIRES esp_https_ota)
endif()

if(CONFIG_JIM_EXT_SPI)
    list(APPEND JIMTCL_ESP32_SRCS
        "${CMAKE_CURRENT_LIST_DIR}/extensions/jim-spi.c")
endif()

if(CONFIG_JIM_EXT_ONEWIRE)
    list(APPEND JIMTCL_ESP32_SRCS
        "${CMAKE_CURRENT_LIST_DIR}/extensions/jim-onewire.c")
endif()

if(CONFIG_JIM_EXT_CRON)
    list(APPEND JIMTCL_ESP32_SRCS
        "${CMAKE_CURRENT_LIST_DIR}/extensions/jim-cron.c")
endif()

if(CONFIG_JIM_EXT_SYSLOG)
    list(APPEND JIMTCL_ESP32_SRCS
        "${CMAKE_CURRENT_LIST_DIR}/extensions/jim-syslog.c")
endif()

if(CONFIG_JIM_EXT_TEST)
    list(APPEND JIMTCL_ESP32_SRCS
        "${CMAKE_CURRENT_LIST_DIR}/extensions/jim-test.c")
endif()

if(CONFIG_JIM_EXT_TCP)
    list(APPEND JIMTCL_ESP32_SRCS
        "${CMAKE_CURRENT_LIST_DIR}/extensions/jim-tcp.c")
endif()

if(CONFIG_JIM_EXT_BLE)
    list(APPEND JIMTCL_ESP32_SRCS
        "${CMAKE_CURRENT_LIST_DIR}/extensions/jim-ble.c")
    list(APPEND JIMTCL_REQUIRES bt)
endif()

if(CONFIG_JIM_EXT_ESPNOW)
    list(APPEND JIMTCL_ESP32_SRCS
        "${CMAKE_CURRENT_LIST_DIR}/extensions/jim-espnow.c")
    list(APPEND JIMTCL_REQUIRES esp_wifi)
endif()

if(CONFIG_JIM_EXT_MDNS)
    list(APPEND JIMTCL_ESP32_SRCS
        "${CMAKE_CURRENT_LIST_DIR}/extensions/jim-mdns.c")
endif()

if(CONFIG_JIM_EXT_SNTP)
    list(APPEND JIMTCL_ESP32_SRCS
        "${CMAKE_CURRENT_LIST_DIR}/extensions/jim-sntp.c")
endif()

if(CONFIG_JIM_EXT_MODBUS)
    list(APPEND JIMTCL_ESP32_SRCS
        "${CMAKE_CURRENT_LIST_DIR}/extensions/jim-modbus.c")
endif()

if(CONFIG_JIM_EXT_MPACK)
    list(APPEND JIMTCL_ESP32_SRCS
        "${CMAKE_CURRENT_LIST_DIR}/extensions/jim-mpack.c"
        "${CMAKE_CURRENT_LIST_DIR}/mpack/mpack.c"
        "${CMAKE_CURRENT_LIST_DIR}/cobs.c")
endif()

if(CONFIG_JIM_EXT_CTLPLANE)
    list(APPEND JIMTCL_ESP32_SRCS
        "${CMAKE_CURRENT_LIST_DIR}/extensions/jim-ctlplane.c")
endif()

if(CONFIG_JIM_EXT_RMT)
    list(APPEND JIMTCL_ESP32_SRCS
        "${CMAKE_CURRENT_LIST_DIR}/extensions/jim-rmt.c")
endif()

if(CONFIG_JIM_EXT_PCNT)
    list(APPEND JIMTCL_ESP32_SRCS
        "${CMAKE_CURRENT_LIST_DIR}/extensions/jim-pcnt.c")
endif()

if(CONFIG_JIM_EXT_MCPWM)
    list(APPEND JIMTCL_ESP32_SRCS
        "${CMAKE_CURRENT_LIST_DIR}/extensions/jim-mcpwm.c")
endif()

idf_component_register(
    SRCS ${JIMTCL_CORE_SRCS} ${JIMTCL_ESP32_SRCS}
    INCLUDE_DIRS
        "${CMAKE_CURRENT_LIST_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/mpack"
        "${JIMTCL_ROOT}"
    REQUIRES ${JIMTCL_REQUIRES}
)

# Compile definitions for ESP32 target
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    JIM_ESP32
)

# Suppress warnings in upstream Jim Tcl code that we don't want to fix
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-parameter
    -Wno-sign-compare
    -Wno-old-style-declaration
    -Wno-implicit-fallthrough
    -Wno-missing-field-initializers
)
