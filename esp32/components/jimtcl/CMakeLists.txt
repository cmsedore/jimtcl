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
)

# IEEE 802.15.4 is only available on ESP32-C6 and ESP32-H2
if(IDF_TARGET STREQUAL "esp32c6" OR IDF_TARGET STREQUAL "esp32h2")
    list(APPEND JIMTCL_ESP32_SRCS
        "${CMAKE_CURRENT_LIST_DIR}/extensions/jim-ieee802154.c")
    list(APPEND JIMTCL_REQUIRES ieee802154)
endif()

idf_component_register(
    SRCS ${JIMTCL_CORE_SRCS} ${JIMTCL_ESP32_SRCS}
    INCLUDE_DIRS
        "${CMAKE_CURRENT_LIST_DIR}"
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
)
