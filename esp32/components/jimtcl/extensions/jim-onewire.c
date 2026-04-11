/* Jim Tcl OneWire Extension for ESP32
 *
 * Bitbang 1-Wire protocol over GPIO with precise timing via esp_rom_delay_us().
 *
 *   onewire init <pin>
 *   onewire reset <pin>                 ;# returns 1 if presence detected
 *   onewire search <pin>                ;# scan bus, return list of 64-bit ROM codes
 *   onewire read_temp <pin> <rom>       ;# DS18B20: convert + read (Celsius)
 *   onewire read_bytes <pin> <count>    ;# raw read N bytes, return as byte list
 *   onewire write_bytes <pin> <bytes>   ;# raw write byte list
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "jim.h"
#include "jim-subcmd.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "jim-onewire";

/* -----------------------------------------------------------------------
 * Low-level 1-Wire bit operations (bitbang)
 *
 * All timing-critical paths disable interrupts briefly to ensure accurate
 * microsecond delays on the GPIO bus.
 * ----------------------------------------------------------------------- */

static int ow_get_pin(Jim_Interp *interp, Jim_Obj *obj, gpio_num_t *pin)
{
    long val;
    if (Jim_GetLong(interp, obj, &val) != JIM_OK) {
        return JIM_ERR;
    }
    if (val < 0 || val >= GPIO_NUM_MAX) {
        Jim_SetResultFormatted(interp, "invalid GPIO pin number: %ld", val);
        return JIM_ERR;
    }
    *pin = (gpio_num_t)val;
    return JIM_OK;
}

/* Drive pin low */
static inline void ow_low(gpio_num_t pin)
{
    gpio_set_level(pin, 0);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT_OD);
}

/* Release pin (float high via pullup) */
static inline void ow_release(gpio_num_t pin)
{
    gpio_set_direction(pin, GPIO_MODE_INPUT);
}

/* Read pin state */
static inline int ow_read_pin(gpio_num_t pin)
{
    return gpio_get_level(pin);
}

/* 1-Wire reset pulse. Returns 1 if a device asserted presence, 0 otherwise. */
static int ow_reset(gpio_num_t pin)
{
    int presence;

    portDISABLE_INTERRUPTS();
    ow_low(pin);
    esp_rom_delay_us(480);
    ow_release(pin);
    esp_rom_delay_us(60);
    presence = (ow_read_pin(pin) == 0) ? 1 : 0;
    portENABLE_INTERRUPTS();

    esp_rom_delay_us(420);
    return presence;
}

/* Write a single bit */
static void ow_write_bit(gpio_num_t pin, int bit)
{
    portDISABLE_INTERRUPTS();
    ow_low(pin);
    if (bit) {
        esp_rom_delay_us(6);
        ow_release(pin);
        esp_rom_delay_us(64);
    } else {
        esp_rom_delay_us(60);
        ow_release(pin);
        esp_rom_delay_us(10);
    }
    portENABLE_INTERRUPTS();
}

/* Read a single bit */
static int ow_read_bit(gpio_num_t pin)
{
    int bit;

    portDISABLE_INTERRUPTS();
    ow_low(pin);
    esp_rom_delay_us(2);
    ow_release(pin);
    esp_rom_delay_us(9);
    bit = ow_read_pin(pin);
    esp_rom_delay_us(55);
    portENABLE_INTERRUPTS();

    return bit;
}

/* Write a byte (LSB first) */
static void ow_write_byte(gpio_num_t pin, uint8_t byte)
{
    for (int i = 0; i < 8; i++) {
        ow_write_bit(pin, (byte >> i) & 1);
    }
}

/* Read a byte (LSB first) */
static uint8_t ow_read_byte(gpio_num_t pin)
{
    uint8_t byte = 0;
    for (int i = 0; i < 8; i++) {
        if (ow_read_bit(pin)) {
            byte |= (1 << i);
        }
    }
    return byte;
}

/* -----------------------------------------------------------------------
 * CRC-8 (Dallas/Maxim polynomial x^8 + x^5 + x^4 + 1)
 * ----------------------------------------------------------------------- */

static uint8_t ow_crc8(const uint8_t *data, int len)
{
    uint8_t crc = 0;
    for (int i = 0; i < len; i++) {
        uint8_t byte = data[i];
        for (int j = 0; j < 8; j++) {
            uint8_t mix = (crc ^ byte) & 0x01;
            crc >>= 1;
            if (mix) crc ^= 0x8C;
            byte >>= 1;
        }
    }
    return crc;
}

/* -----------------------------------------------------------------------
 * 1-Wire ROM Search Algorithm (0xF0)
 *
 * Discovers all devices on the bus. Returns the number of devices found.
 * ROM codes are stored in roms[] (caller must provide space for max_roms
 * entries of 8 bytes each).
 * ----------------------------------------------------------------------- */

#define OW_MAX_DEVICES 8

static int ow_search(gpio_num_t pin, uint8_t roms[][8], int max_roms)
{
    int device_count = 0;
    int last_discrepancy = 0;
    int last_device = 0;
    uint8_t rom[8];

    memset(rom, 0, sizeof(rom));

    while (!last_device && device_count < max_roms) {
        if (!ow_reset(pin)) {
            break;  /* No presence */
        }

        ow_write_byte(pin, 0xF0);  /* Search ROM command */

        int discrepancy_marker = 0;

        for (int bit_idx = 1; bit_idx <= 64; bit_idx++) {
            int byte_idx = (bit_idx - 1) / 8;
            int bit_mask = 1 << ((bit_idx - 1) % 8);

            int id_bit = ow_read_bit(pin);
            int cmp_bit = ow_read_bit(pin);

            if (id_bit == 1 && cmp_bit == 1) {
                /* No devices responding */
                break;
            }

            int direction;
            if (id_bit != cmp_bit) {
                /* All devices have the same bit here */
                direction = id_bit;
            } else {
                /* Discrepancy: both 0 and 1 exist */
                if (bit_idx == last_discrepancy) {
                    direction = 1;
                } else if (bit_idx > last_discrepancy) {
                    direction = 0;
                } else {
                    direction = (rom[byte_idx] & bit_mask) ? 1 : 0;
                }
                if (direction == 0) {
                    discrepancy_marker = bit_idx;
                }
            }

            if (direction) {
                rom[byte_idx] |= bit_mask;
            } else {
                rom[byte_idx] &= ~bit_mask;
            }

            ow_write_bit(pin, direction);
        }

        /* Verify CRC */
        if (ow_crc8(rom, 7) == rom[7]) {
            memcpy(roms[device_count], rom, 8);
            device_count++;
        }

        last_discrepancy = discrepancy_marker;
        if (last_discrepancy == 0) {
            last_device = 1;
        }
    }

    return device_count;
}

/* -----------------------------------------------------------------------
 * Parse a hex ROM string "28FF12345678AB" into 8 bytes
 * ----------------------------------------------------------------------- */

static int parse_rom_hex(const char *hex, uint8_t rom[8])
{
    if (strlen(hex) != 16) return -1;
    for (int i = 0; i < 8; i++) {
        unsigned int byte_val;
        if (sscanf(hex + i * 2, "%2x", &byte_val) != 1) return -1;
        rom[i] = (uint8_t)byte_val;
    }
    return 0;
}

/* Format 8-byte ROM as hex string */
static void format_rom_hex(const uint8_t rom[8], char *out)
{
    for (int i = 0; i < 8; i++) {
        sprintf(out + i * 2, "%02X", rom[i]);
    }
    out[16] = '\0';
}

/* -----------------------------------------------------------------------
 * Tcl subcommands
 * ----------------------------------------------------------------------- */

static int onewire_cmd_init(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    gpio_num_t pin;
    if (ow_get_pin(interp, argv[0], &pin) != JIM_OK) {
        return JIM_ERR;
    }

    /* Configure as open-drain output + input with internal pullup */
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_pullup_en(pin);
    gpio_set_level(pin, 1);

    ESP_LOGI(TAG, "OneWire initialized on GPIO %d (open-drain + pullup)", (int)pin);
    return JIM_OK;
}

static int onewire_cmd_reset(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    gpio_num_t pin;
    if (ow_get_pin(interp, argv[0], &pin) != JIM_OK) {
        return JIM_ERR;
    }

    int presence = ow_reset(pin);
    Jim_SetResultInt(interp, presence);
    return JIM_OK;
}

static int onewire_cmd_search(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    gpio_num_t pin;
    if (ow_get_pin(interp, argv[0], &pin) != JIM_OK) {
        return JIM_ERR;
    }

    uint8_t roms[OW_MAX_DEVICES][8];
    int count = ow_search(pin, roms, OW_MAX_DEVICES);

    Jim_Obj *result = Jim_NewListObj(interp, NULL, 0);
    for (int i = 0; i < count; i++) {
        char hex[17];
        format_rom_hex(roms[i], hex);
        Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, hex, -1));
    }

    Jim_SetResult(interp, result);
    return JIM_OK;
}

static int onewire_cmd_read_temp(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    gpio_num_t pin;
    if (ow_get_pin(interp, argv[0], &pin) != JIM_OK) {
        return JIM_ERR;
    }

    const char *rom_str = Jim_String(argv[1]);
    int use_skip_rom = (strcmp(rom_str, "all") == 0);
    uint8_t rom[8];

    if (!use_skip_rom) {
        if (parse_rom_hex(rom_str, rom) != 0) {
            Jim_SetResultFormatted(interp,
                "invalid ROM code \"%s\": must be 16 hex chars or \"all\"", rom_str);
            return JIM_ERR;
        }
        /* Verify it is a DS18B20 family code (0x28) */
        if (rom[0] != 0x28) {
            Jim_SetResultFormatted(interp,
                "ROM family code 0x%02X is not a DS18B20 (expected 0x28)", rom[0]);
            return JIM_ERR;
        }
    }

    /* Step 1: Issue Convert T command */
    if (!ow_reset(pin)) {
        Jim_SetResultString(interp, "no device presence detected", -1);
        return JIM_ERR;
    }

    if (use_skip_rom) {
        ow_write_byte(pin, 0xCC);  /* Skip ROM */
    } else {
        ow_write_byte(pin, 0x55);  /* Match ROM */
        for (int i = 0; i < 8; i++) {
            ow_write_byte(pin, rom[i]);
        }
    }
    ow_write_byte(pin, 0x44);  /* Convert T */

    /* Wait for conversion (750ms for 12-bit resolution) */
    vTaskDelay(pdMS_TO_TICKS(750));

    /* Step 2: Read Scratchpad */
    if (!ow_reset(pin)) {
        Jim_SetResultString(interp, "no device presence after conversion", -1);
        return JIM_ERR;
    }

    if (use_skip_rom) {
        ow_write_byte(pin, 0xCC);  /* Skip ROM */
    } else {
        ow_write_byte(pin, 0x55);  /* Match ROM */
        for (int i = 0; i < 8; i++) {
            ow_write_byte(pin, rom[i]);
        }
    }
    ow_write_byte(pin, 0xBE);  /* Read Scratchpad */

    uint8_t scratchpad[9];
    for (int i = 0; i < 9; i++) {
        scratchpad[i] = ow_read_byte(pin);
    }

    /* Verify CRC of scratchpad */
    if (ow_crc8(scratchpad, 8) != scratchpad[8]) {
        Jim_SetResultString(interp, "scratchpad CRC mismatch", -1);
        return JIM_ERR;
    }

    /* Parse 16-bit signed temperature (12-bit resolution: 0.0625 per LSB) */
    int16_t raw = (int16_t)((scratchpad[1] << 8) | scratchpad[0]);
    float temp_c = raw / 16.0f;

    char temp_str[16];
    snprintf(temp_str, sizeof(temp_str), "%.4f", temp_c);

    /* Trim trailing zeros but keep at least one decimal */
    int len = strlen(temp_str);
    while (len > 1 && temp_str[len - 1] == '0' && temp_str[len - 2] != '.') {
        temp_str[--len] = '\0';
    }

    Jim_SetResultString(interp, temp_str, -1);
    return JIM_OK;
}

static int onewire_cmd_read_bytes(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    gpio_num_t pin;
    if (ow_get_pin(interp, argv[0], &pin) != JIM_OK) {
        return JIM_ERR;
    }

    long count;
    if (Jim_GetLong(interp, argv[1], &count) != JIM_OK) {
        return JIM_ERR;
    }
    if (count <= 0 || count > 256) {
        Jim_SetResultString(interp, "count must be 1-256", -1);
        return JIM_ERR;
    }

    Jim_Obj *result = Jim_NewListObj(interp, NULL, 0);
    for (long i = 0; i < count; i++) {
        Jim_ListAppendElement(interp, result, Jim_NewIntObj(interp, ow_read_byte(pin)));
    }

    Jim_SetResult(interp, result);
    return JIM_OK;
}

static int onewire_cmd_write_bytes(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    gpio_num_t pin;
    if (ow_get_pin(interp, argv[0], &pin) != JIM_OK) {
        return JIM_ERR;
    }

    int data_len = Jim_ListLength(interp, argv[1]);
    if (data_len <= 0) {
        Jim_SetResultString(interp, "empty byte list", -1);
        return JIM_ERR;
    }

    for (int i = 0; i < data_len; i++) {
        Jim_Obj *elem = Jim_ListGetIndex(interp, argv[1], i);
        long byte_val;
        if (Jim_GetLong(interp, elem, &byte_val) != JIM_OK) {
            return JIM_ERR;
        }
        ow_write_byte(pin, (uint8_t)(byte_val & 0xFF));
    }

    Jim_SetResultInt(interp, data_len);
    return JIM_OK;
}

/* -----------------------------------------------------------------------
 * Subcommand dispatch table
 * ----------------------------------------------------------------------- */

static const jim_subcmd_type onewire_command_table[] = {
    {   "init",
        "pin",
        onewire_cmd_init,
        1,
        1,
        /* Description: Initialize GPIO for OneWire (open-drain + pullup) */
    },
    {   "reset",
        "pin",
        onewire_cmd_reset,
        1,
        1,
        /* Description: Send reset pulse, return 1 if presence detected */
    },
    {   "search",
        "pin",
        onewire_cmd_search,
        1,
        1,
        /* Description: Scan bus and return list of ROM code hex strings */
    },
    {   "read_temp",
        "pin rom",
        onewire_cmd_read_temp,
        2,
        2,
        /* Description: DS18B20: start conversion and read temperature (Celsius) */
    },
    {   "read_bytes",
        "pin count",
        onewire_cmd_read_bytes,
        2,
        2,
        /* Description: Read N raw bytes from bus, return as byte list */
    },
    {   "write_bytes",
        "pin bytes",
        onewire_cmd_write_bytes,
        2,
        2,
        /* Description: Write byte list to bus */
    },
    { NULL }
};

int Jim_onewireInit(Jim_Interp *interp)
{
    Jim_PackageProvideCheck(interp, "onewire");
    Jim_RegisterSubCmd(interp, "onewire", onewire_command_table, NULL);
    return JIM_OK;
}
