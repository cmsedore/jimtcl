/* Jim Tcl Modbus RTU Master Extension for ESP32
 *
 * Provides Tcl commands for Modbus RTU over UART:
 *
 *   modbus init <uart_port> -tx <pin> -rx <pin> -baud <rate> ?-de_pin <pin>?
 *   modbus read_holding <slave_id> <start_reg> <count>     ;# FC03
 *   modbus read_input <slave_id> <start_reg> <count>       ;# FC04
 *   modbus write_single <slave_id> <reg> <value>            ;# FC06
 *   modbus write_multiple <slave_id> <start_reg> <values>   ;# FC16
 *   modbus read_coils <slave_id> <start> <count>            ;# FC01
 *   modbus write_coil <slave_id> <coil> <value>             ;# FC05
 *   modbus deinit
 *   modbus status
 *
 * Implements raw Modbus RTU frame building/parsing with CRC16 over UART.
 * No external Modbus library required.
 */

#include <string.h>
#include <stdlib.h>
#include "jim.h"
#include "jim-subcmd.h"

#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "jim-modbus";

#define MODBUS_MAX_INSTANCES   2
#define MODBUS_FRAME_MAX       256
#define MODBUS_RESPONSE_TIMEOUT_MS  1000
#define MODBUS_RX_BUF_SIZE    512
#define MODBUS_TX_BUF_SIZE    512

/* Modbus function codes */
#define FC_READ_COILS           0x01
#define FC_READ_HOLDING_REGS    0x03
#define FC_READ_INPUT_REGS      0x04
#define FC_WRITE_SINGLE_COIL    0x05
#define FC_WRITE_SINGLE_REG     0x06
#define FC_WRITE_MULTIPLE_REGS  0x10

typedef struct {
    int initialized;
    uart_port_t uart_num;
    int tx_pin;
    int rx_pin;
    int de_pin;         /* RS485 direction enable pin, -1 if unused */
    int baud_rate;
} modbus_instance_t;

static modbus_instance_t modbus_inst[MODBUS_MAX_INSTANCES] = { 0 };
static int modbus_active_port = -1;  /* Currently selected instance index */

/* ---------------------------------------------------------------------------
 * CRC16 (Modbus): polynomial 0xA001 (reflected 0x8005)
 * ---------------------------------------------------------------------------*/

static uint16_t modbus_crc16(const uint8_t *data, int len)
{
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

/* ---------------------------------------------------------------------------
 * Send a Modbus RTU frame and receive response
 * ---------------------------------------------------------------------------*/

static int modbus_transact(modbus_instance_t *inst, const uint8_t *frame, int frame_len,
                           uint8_t *resp, int resp_max, int *resp_len)
{
    /* Flush any stale data in RX buffer */
    uart_flush_input(inst->uart_num);

    /* Send frame */
    int written = uart_write_bytes(inst->uart_num, (const char *)frame, frame_len);
    if (written != frame_len) {
        ESP_LOGE(TAG, "UART write failed: wrote %d of %d", written, frame_len);
        return -1;
    }

    /* Wait for TX to complete */
    uart_wait_tx_done(inst->uart_num, pdMS_TO_TICKS(100));

    /* Inter-frame delay: 3.5 character times at baud rate.
     * At 9600 baud, 1 char ~ 1.04ms, so 3.5 chars ~ 4ms.
     * Use a minimum of 5ms for safety. */
    int char_time_us = (1000000 * 11) / inst->baud_rate;  /* 11 bits per char */
    int delay_ms = (char_time_us * 4) / 1000;
    if (delay_ms < 5) delay_ms = 5;

    /* Read response with timeout */
    *resp_len = uart_read_bytes(inst->uart_num, resp, resp_max,
                                pdMS_TO_TICKS(MODBUS_RESPONSE_TIMEOUT_MS));
    if (*resp_len <= 0) {
        ESP_LOGW(TAG, "No response from slave (timeout)");
        return -2;
    }

    /* Validate minimum response: slave_id(1) + fc(1) + data(1+) + crc(2) = 5 */
    if (*resp_len < 5) {
        ESP_LOGW(TAG, "Response too short: %d bytes", *resp_len);
        return -3;
    }

    /* Validate CRC */
    uint16_t recv_crc = resp[*resp_len - 2] | (resp[*resp_len - 1] << 8);
    uint16_t calc_crc = modbus_crc16(resp, *resp_len - 2);
    if (recv_crc != calc_crc) {
        ESP_LOGW(TAG, "CRC mismatch: recv=0x%04X calc=0x%04X", recv_crc, calc_crc);
        return -4;
    }

    /* Check for Modbus exception response (FC | 0x80) */
    if (resp[1] & 0x80) {
        ESP_LOGW(TAG, "Modbus exception: FC=0x%02X code=%d", resp[1], resp[2]);
        return -5;
    }

    return 0;
}

/* ---------------------------------------------------------------------------
 * Find instance by active port or return error
 * ---------------------------------------------------------------------------*/

static modbus_instance_t *get_active_instance(Jim_Interp *interp)
{
    if (modbus_active_port < 0 || modbus_active_port >= MODBUS_MAX_INSTANCES ||
        !modbus_inst[modbus_active_port].initialized) {
        Jim_SetResultString(interp, "modbus not initialized", -1);
        return NULL;
    }
    return &modbus_inst[modbus_active_port];
}

/* ---------------------------------------------------------------------------
 * Command handlers
 * ---------------------------------------------------------------------------*/

static int modbus_cmd_init(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc < 1) {
        Jim_SetResultString(interp, "usage: modbus init <uart_port> -tx <pin> -rx <pin> -baud <rate> ?-de_pin <pin>?", -1);
        return JIM_ERR;
    }

    long uart_port;
    if (Jim_GetLong(interp, argv[0], &uart_port) != JIM_OK) return JIM_ERR;

    if (uart_port < 0 || uart_port >= MODBUS_MAX_INSTANCES) {
        Jim_SetResultFormatted(interp, "uart_port must be 0-%d", MODBUS_MAX_INSTANCES - 1);
        return JIM_ERR;
    }

    if (modbus_inst[uart_port].initialized) {
        Jim_SetResultString(interp, "already initialized on this port", -1);
        return JIM_ERR;
    }

    long tx_pin = -1, rx_pin = -1, baud = 9600, de_pin = -1;

    for (int i = 1; i < argc; i++) {
        const char *opt = Jim_String(argv[i]);
        if (strcmp(opt, "-tx") == 0 && i + 1 < argc) {
            if (Jim_GetLong(interp, argv[++i], &tx_pin) != JIM_OK) return JIM_ERR;
        }
        else if (strcmp(opt, "-rx") == 0 && i + 1 < argc) {
            if (Jim_GetLong(interp, argv[++i], &rx_pin) != JIM_OK) return JIM_ERR;
        }
        else if (strcmp(opt, "-baud") == 0 && i + 1 < argc) {
            if (Jim_GetLong(interp, argv[++i], &baud) != JIM_OK) return JIM_ERR;
        }
        else if (strcmp(opt, "-de_pin") == 0 && i + 1 < argc) {
            if (Jim_GetLong(interp, argv[++i], &de_pin) != JIM_OK) return JIM_ERR;
        }
        else {
            Jim_SetResultFormatted(interp, "unknown option \"%s\"", opt);
            return JIM_ERR;
        }
    }

    if (tx_pin < 0 || rx_pin < 0) {
        Jim_SetResultString(interp, "-tx and -rx pins are required", -1);
        return JIM_ERR;
    }

    /* Configure UART */
    uart_config_t uart_config = {
        .baud_rate = (int)baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install((uart_port_t)uart_port, MODBUS_RX_BUF_SIZE,
                                         MODBUS_TX_BUF_SIZE, 0, NULL, 0);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "uart_driver_install failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    err = uart_param_config((uart_port_t)uart_port, &uart_config);
    if (err != ESP_OK) {
        uart_driver_delete((uart_port_t)uart_port);
        Jim_SetResultFormatted(interp, "uart_param_config failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    err = uart_set_pin((uart_port_t)uart_port, (int)tx_pin, (int)rx_pin,
                        de_pin >= 0 ? (int)de_pin : UART_PIN_NO_CHANGE,
                        UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        uart_driver_delete((uart_port_t)uart_port);
        Jim_SetResultFormatted(interp, "uart_set_pin failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    /* Enable RS485 half-duplex mode if DE pin specified */
    if (de_pin >= 0) {
        uart_set_mode((uart_port_t)uart_port, UART_MODE_RS485_HALF_DUPLEX);
    }

    modbus_inst[uart_port].initialized = 1;
    modbus_inst[uart_port].uart_num = (uart_port_t)uart_port;
    modbus_inst[uart_port].tx_pin = (int)tx_pin;
    modbus_inst[uart_port].rx_pin = (int)rx_pin;
    modbus_inst[uart_port].de_pin = (int)de_pin;
    modbus_inst[uart_port].baud_rate = (int)baud;

    modbus_active_port = (int)uart_port;

    ESP_LOGI(TAG, "Modbus RTU initialized on UART%ld (TX:%ld RX:%ld baud:%ld)",
             uart_port, tx_pin, rx_pin, baud);
    return JIM_OK;
}

static int modbus_cmd_read_holding(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    modbus_instance_t *inst = get_active_instance(interp);
    if (!inst) return JIM_ERR;

    if (argc < 3) {
        Jim_SetResultString(interp, "usage: modbus read_holding <slave_id> <start_reg> <count>", -1);
        return JIM_ERR;
    }

    long slave_id, start_reg, count;
    if (Jim_GetLong(interp, argv[0], &slave_id) != JIM_OK) return JIM_ERR;
    if (Jim_GetLong(interp, argv[1], &start_reg) != JIM_OK) return JIM_ERR;
    if (Jim_GetLong(interp, argv[2], &count) != JIM_OK) return JIM_ERR;

    /* Build request: [slave_id, 0x03, start_hi, start_lo, count_hi, count_lo, crc_lo, crc_hi] */
    uint8_t frame[8];
    frame[0] = (uint8_t)slave_id;
    frame[1] = FC_READ_HOLDING_REGS;
    frame[2] = (uint8_t)(start_reg >> 8);
    frame[3] = (uint8_t)(start_reg & 0xFF);
    frame[4] = (uint8_t)(count >> 8);
    frame[5] = (uint8_t)(count & 0xFF);
    uint16_t crc = modbus_crc16(frame, 6);
    frame[6] = crc & 0xFF;
    frame[7] = (crc >> 8) & 0xFF;

    uint8_t resp[MODBUS_FRAME_MAX];
    int resp_len = 0;
    int rc = modbus_transact(inst, frame, 8, resp, sizeof(resp), &resp_len);
    if (rc != 0) {
        Jim_SetResultFormatted(interp, "modbus transaction failed (error %d)", rc);
        return JIM_ERR;
    }

    /* Response: [slave_id, 0x03, byte_count, data..., crc_lo, crc_hi] */
    int byte_count = resp[2];
    int reg_count = byte_count / 2;

    Jim_Obj *result = Jim_NewListObj(interp, NULL, 0);
    for (int i = 0; i < reg_count; i++) {
        uint16_t val = (resp[3 + i * 2] << 8) | resp[3 + i * 2 + 1];
        Jim_ListAppendElement(interp, result, Jim_NewIntObj(interp, val));
    }
    Jim_SetResult(interp, result);
    return JIM_OK;
}

static int modbus_cmd_read_input(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    modbus_instance_t *inst = get_active_instance(interp);
    if (!inst) return JIM_ERR;

    if (argc < 3) {
        Jim_SetResultString(interp, "usage: modbus read_input <slave_id> <start_reg> <count>", -1);
        return JIM_ERR;
    }

    long slave_id, start_reg, count;
    if (Jim_GetLong(interp, argv[0], &slave_id) != JIM_OK) return JIM_ERR;
    if (Jim_GetLong(interp, argv[1], &start_reg) != JIM_OK) return JIM_ERR;
    if (Jim_GetLong(interp, argv[2], &count) != JIM_OK) return JIM_ERR;

    uint8_t frame[8];
    frame[0] = (uint8_t)slave_id;
    frame[1] = FC_READ_INPUT_REGS;
    frame[2] = (uint8_t)(start_reg >> 8);
    frame[3] = (uint8_t)(start_reg & 0xFF);
    frame[4] = (uint8_t)(count >> 8);
    frame[5] = (uint8_t)(count & 0xFF);
    uint16_t crc = modbus_crc16(frame, 6);
    frame[6] = crc & 0xFF;
    frame[7] = (crc >> 8) & 0xFF;

    uint8_t resp[MODBUS_FRAME_MAX];
    int resp_len = 0;
    int rc = modbus_transact(inst, frame, 8, resp, sizeof(resp), &resp_len);
    if (rc != 0) {
        Jim_SetResultFormatted(interp, "modbus transaction failed (error %d)", rc);
        return JIM_ERR;
    }

    int byte_count = resp[2];
    int reg_count = byte_count / 2;

    Jim_Obj *result = Jim_NewListObj(interp, NULL, 0);
    for (int i = 0; i < reg_count; i++) {
        uint16_t val = (resp[3 + i * 2] << 8) | resp[3 + i * 2 + 1];
        Jim_ListAppendElement(interp, result, Jim_NewIntObj(interp, val));
    }
    Jim_SetResult(interp, result);
    return JIM_OK;
}

static int modbus_cmd_write_single(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    modbus_instance_t *inst = get_active_instance(interp);
    if (!inst) return JIM_ERR;

    if (argc < 3) {
        Jim_SetResultString(interp, "usage: modbus write_single <slave_id> <reg> <value>", -1);
        return JIM_ERR;
    }

    long slave_id, reg, value;
    if (Jim_GetLong(interp, argv[0], &slave_id) != JIM_OK) return JIM_ERR;
    if (Jim_GetLong(interp, argv[1], &reg) != JIM_OK) return JIM_ERR;
    if (Jim_GetLong(interp, argv[2], &value) != JIM_OK) return JIM_ERR;

    /* FC06: [slave_id, 0x06, reg_hi, reg_lo, val_hi, val_lo, crc_lo, crc_hi] */
    uint8_t frame[8];
    frame[0] = (uint8_t)slave_id;
    frame[1] = FC_WRITE_SINGLE_REG;
    frame[2] = (uint8_t)(reg >> 8);
    frame[3] = (uint8_t)(reg & 0xFF);
    frame[4] = (uint8_t)(value >> 8);
    frame[5] = (uint8_t)(value & 0xFF);
    uint16_t crc = modbus_crc16(frame, 6);
    frame[6] = crc & 0xFF;
    frame[7] = (crc >> 8) & 0xFF;

    uint8_t resp[MODBUS_FRAME_MAX];
    int resp_len = 0;
    int rc = modbus_transact(inst, frame, 8, resp, sizeof(resp), &resp_len);
    if (rc != 0) {
        Jim_SetResultFormatted(interp, "modbus transaction failed (error %d)", rc);
        return JIM_ERR;
    }

    return JIM_OK;
}

static int modbus_cmd_write_multiple(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    modbus_instance_t *inst = get_active_instance(interp);
    if (!inst) return JIM_ERR;

    if (argc < 3) {
        Jim_SetResultString(interp, "usage: modbus write_multiple <slave_id> <start_reg> <values_list>", -1);
        return JIM_ERR;
    }

    long slave_id, start_reg;
    if (Jim_GetLong(interp, argv[0], &slave_id) != JIM_OK) return JIM_ERR;
    if (Jim_GetLong(interp, argv[1], &start_reg) != JIM_OK) return JIM_ERR;

    Jim_Obj *valuesList = argv[2];
    int reg_count = Jim_ListLength(interp, valuesList);
    if (reg_count <= 0 || reg_count > 123) {
        Jim_SetResultString(interp, "register count must be 1-123", -1);
        return JIM_ERR;
    }

    int byte_count = reg_count * 2;
    /* FC16: [slave_id, 0x10, start_hi, start_lo, count_hi, count_lo, byte_count, data..., crc] */
    int frame_len = 7 + byte_count + 2;
    uint8_t *frame = malloc(frame_len);
    if (!frame) {
        Jim_SetResultString(interp, "out of memory", -1);
        return JIM_ERR;
    }

    frame[0] = (uint8_t)slave_id;
    frame[1] = FC_WRITE_MULTIPLE_REGS;
    frame[2] = (uint8_t)(start_reg >> 8);
    frame[3] = (uint8_t)(start_reg & 0xFF);
    frame[4] = (uint8_t)(reg_count >> 8);
    frame[5] = (uint8_t)(reg_count & 0xFF);
    frame[6] = (uint8_t)byte_count;

    for (int i = 0; i < reg_count; i++) {
        long val;
        if (Jim_GetLong(interp, Jim_ListGetIndex(interp, valuesList, i), &val) != JIM_OK) {
            free(frame);
            return JIM_ERR;
        }
        frame[7 + i * 2] = (uint8_t)(val >> 8);
        frame[7 + i * 2 + 1] = (uint8_t)(val & 0xFF);
    }

    uint16_t crc = modbus_crc16(frame, frame_len - 2);
    frame[frame_len - 2] = crc & 0xFF;
    frame[frame_len - 1] = (crc >> 8) & 0xFF;

    uint8_t resp[MODBUS_FRAME_MAX];
    int resp_len = 0;
    int rc = modbus_transact(inst, frame, frame_len, resp, sizeof(resp), &resp_len);
    free(frame);

    if (rc != 0) {
        Jim_SetResultFormatted(interp, "modbus transaction failed (error %d)", rc);
        return JIM_ERR;
    }

    return JIM_OK;
}

static int modbus_cmd_read_coils(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    modbus_instance_t *inst = get_active_instance(interp);
    if (!inst) return JIM_ERR;

    if (argc < 3) {
        Jim_SetResultString(interp, "usage: modbus read_coils <slave_id> <start> <count>", -1);
        return JIM_ERR;
    }

    long slave_id, start, count;
    if (Jim_GetLong(interp, argv[0], &slave_id) != JIM_OK) return JIM_ERR;
    if (Jim_GetLong(interp, argv[1], &start) != JIM_OK) return JIM_ERR;
    if (Jim_GetLong(interp, argv[2], &count) != JIM_OK) return JIM_ERR;

    /* FC01: [slave_id, 0x01, start_hi, start_lo, count_hi, count_lo, crc_lo, crc_hi] */
    uint8_t frame[8];
    frame[0] = (uint8_t)slave_id;
    frame[1] = FC_READ_COILS;
    frame[2] = (uint8_t)(start >> 8);
    frame[3] = (uint8_t)(start & 0xFF);
    frame[4] = (uint8_t)(count >> 8);
    frame[5] = (uint8_t)(count & 0xFF);
    uint16_t crc = modbus_crc16(frame, 6);
    frame[6] = crc & 0xFF;
    frame[7] = (crc >> 8) & 0xFF;

    uint8_t resp[MODBUS_FRAME_MAX];
    int resp_len = 0;
    int rc = modbus_transact(inst, frame, 8, resp, sizeof(resp), &resp_len);
    if (rc != 0) {
        Jim_SetResultFormatted(interp, "modbus transaction failed (error %d)", rc);
        return JIM_ERR;
    }

    /* Response: [slave_id, 0x01, byte_count, coil_data..., crc] */
    int data_bytes = resp[2];

    Jim_Obj *result = Jim_NewListObj(interp, NULL, 0);
    for (int i = 0; i < (int)count; i++) {
        int byte_idx = i / 8;
        int bit_idx = i % 8;
        if (byte_idx < data_bytes) {
            int val = (resp[3 + byte_idx] >> bit_idx) & 1;
            Jim_ListAppendElement(interp, result, Jim_NewIntObj(interp, val));
        }
    }
    Jim_SetResult(interp, result);
    return JIM_OK;
}

static int modbus_cmd_write_coil(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    modbus_instance_t *inst = get_active_instance(interp);
    if (!inst) return JIM_ERR;

    if (argc < 3) {
        Jim_SetResultString(interp, "usage: modbus write_coil <slave_id> <coil> <value>", -1);
        return JIM_ERR;
    }

    long slave_id, coil, value;
    if (Jim_GetLong(interp, argv[0], &slave_id) != JIM_OK) return JIM_ERR;
    if (Jim_GetLong(interp, argv[1], &coil) != JIM_OK) return JIM_ERR;
    if (Jim_GetLong(interp, argv[2], &value) != JIM_OK) return JIM_ERR;

    /* FC05: coil ON = 0xFF00, OFF = 0x0000 */
    uint16_t coil_val = value ? 0xFF00 : 0x0000;

    uint8_t frame[8];
    frame[0] = (uint8_t)slave_id;
    frame[1] = FC_WRITE_SINGLE_COIL;
    frame[2] = (uint8_t)(coil >> 8);
    frame[3] = (uint8_t)(coil & 0xFF);
    frame[4] = (uint8_t)(coil_val >> 8);
    frame[5] = (uint8_t)(coil_val & 0xFF);
    uint16_t crc = modbus_crc16(frame, 6);
    frame[6] = crc & 0xFF;
    frame[7] = (crc >> 8) & 0xFF;

    uint8_t resp[MODBUS_FRAME_MAX];
    int resp_len = 0;
    int rc = modbus_transact(inst, frame, 8, resp, sizeof(resp), &resp_len);
    if (rc != 0) {
        Jim_SetResultFormatted(interp, "modbus transaction failed (error %d)", rc);
        return JIM_ERR;
    }

    return JIM_OK;
}

static int modbus_cmd_deinit(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    modbus_instance_t *inst = get_active_instance(interp);
    if (!inst) return JIM_ERR;

    uart_driver_delete(inst->uart_num);
    memset(inst, 0, sizeof(*inst));
    modbus_active_port = -1;

    /* Find another initialized instance if any */
    for (int i = 0; i < MODBUS_MAX_INSTANCES; i++) {
        if (modbus_inst[i].initialized) {
            modbus_active_port = i;
            break;
        }
    }

    ESP_LOGI(TAG, "Modbus deinitialized");
    return JIM_OK;
}

static int modbus_cmd_status(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    Jim_Obj *result = Jim_NewListObj(interp, NULL, 0);

    for (int i = 0; i < MODBUS_MAX_INSTANCES; i++) {
        if (modbus_inst[i].initialized) {
            Jim_Obj *entry = Jim_NewListObj(interp, NULL, 0);

            Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, "port", -1));
            Jim_ListAppendElement(interp, entry, Jim_NewIntObj(interp, modbus_inst[i].uart_num));

            Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, "baud", -1));
            Jim_ListAppendElement(interp, entry, Jim_NewIntObj(interp, modbus_inst[i].baud_rate));

            Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, "tx_pin", -1));
            Jim_ListAppendElement(interp, entry, Jim_NewIntObj(interp, modbus_inst[i].tx_pin));

            Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, "rx_pin", -1));
            Jim_ListAppendElement(interp, entry, Jim_NewIntObj(interp, modbus_inst[i].rx_pin));

            if (modbus_inst[i].de_pin >= 0) {
                Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, "de_pin", -1));
                Jim_ListAppendElement(interp, entry, Jim_NewIntObj(interp, modbus_inst[i].de_pin));
            }

            Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, "active", -1));
            Jim_ListAppendElement(interp, entry, Jim_NewIntObj(interp, i == modbus_active_port));

            Jim_ListAppendElement(interp, result, entry);
        }
    }

    if (Jim_ListLength(interp, result) == 0) {
        Jim_SetResultString(interp, "not initialized", -1);
    } else {
        Jim_SetResult(interp, result);
    }
    return JIM_OK;
}

static const jim_subcmd_type modbus_command_table[] = {
    {   "init",
        "uart_port -tx pin -rx pin -baud rate ?-de_pin pin?",
        modbus_cmd_init,
        1,
        -1,
        /* Description: Initialize Modbus RTU on a UART port */
    },
    {   "read_holding",
        "slave_id start_reg count",
        modbus_cmd_read_holding,
        3,
        3,
        /* Description: Read holding registers (FC03) */
    },
    {   "read_input",
        "slave_id start_reg count",
        modbus_cmd_read_input,
        3,
        3,
        /* Description: Read input registers (FC04) */
    },
    {   "write_single",
        "slave_id reg value",
        modbus_cmd_write_single,
        3,
        3,
        /* Description: Write a single register (FC06) */
    },
    {   "write_multiple",
        "slave_id start_reg {values}",
        modbus_cmd_write_multiple,
        3,
        3,
        /* Description: Write multiple registers (FC16) */
    },
    {   "read_coils",
        "slave_id start count",
        modbus_cmd_read_coils,
        3,
        3,
        /* Description: Read coils (FC01) */
    },
    {   "write_coil",
        "slave_id coil value",
        modbus_cmd_write_coil,
        3,
        3,
        /* Description: Write a single coil (FC05) */
    },
    {   "deinit",
        NULL,
        modbus_cmd_deinit,
        0,
        0,
        /* Description: Deinitialize Modbus */
    },
    {   "status",
        NULL,
        modbus_cmd_status,
        0,
        0,
        /* Description: Return Modbus status */
    },
    { NULL }
};

int Jim_modbusInit(Jim_Interp *interp)
{
    Jim_PackageProvideCheck(interp, "modbus");
    Jim_RegisterSubCmd(interp, "modbus", modbus_command_table, NULL);
    return JIM_OK;
}
