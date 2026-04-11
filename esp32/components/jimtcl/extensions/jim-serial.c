/* Jim Tcl Serial (UART) Extension for ESP32
 *
 * Provides Tcl commands for UART serial communication:
 *
 *   serial open <port> -tx <pin> -rx <pin> -baud <rate> ?-databits 5|6|7|8? ?-stopbits 1|1.5|2? ?-parity none|even|odd?
 *   serial write <port> <data>
 *   serial read <port> ?length? ?-timeout ms?
 *   serial readline <port> ?-timeout ms?
 *   serial available <port>
 *   serial listen <port> -callback {proc task}
 *   serial listen <port> -remove
 *   serial close <port>
 */

#include <string.h>
#include <stdlib.h>
#include "jim.h"
#include "jim-subcmd.h"
#include "jim-esp32-task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "jim-serial";

/* ---------------------------------------------------------------------------
 * Static state — track up to 3 UART ports
 * ---------------------------------------------------------------------------*/

#define SERIAL_MAX_PORTS  3
#define SERIAL_RX_BUF     2048
#define SERIAL_TX_BUF     0       /* No TX buffer needed for blocking writes */
#define SERIAL_READ_CHUNK 256

typedef struct {
    int installed;
    TaskHandle_t listener_task;
    volatile int listener_stop;
    char listener_proc[64];
    char listener_target[16];
} serial_port_state_t;

static serial_port_state_t serial_ports[SERIAL_MAX_PORTS] = { {0} };

/* ---------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------------*/

static int serial_get_port(Jim_Interp *interp, Jim_Obj *obj, int *port)
{
    long val;
    if (Jim_GetLong(interp, obj, &val) != JIM_OK) return JIM_ERR;
    if (val < 0 || val >= SERIAL_MAX_PORTS) {
        Jim_SetResultFormatted(interp, "invalid UART port: %ld (must be 0-%d)",
                               val, SERIAL_MAX_PORTS - 1);
        return JIM_ERR;
    }
    *port = (int)val;
    return JIM_OK;
}

static int serial_parse_databits(Jim_Interp *interp, const char *str, uart_word_length_t *bits)
{
    if (strcmp(str, "5") == 0)      *bits = UART_DATA_5_BITS;
    else if (strcmp(str, "6") == 0) *bits = UART_DATA_6_BITS;
    else if (strcmp(str, "7") == 0) *bits = UART_DATA_7_BITS;
    else if (strcmp(str, "8") == 0) *bits = UART_DATA_8_BITS;
    else {
        Jim_SetResultFormatted(interp, "bad databits \"%s\": should be 5, 6, 7, or 8", str);
        return JIM_ERR;
    }
    return JIM_OK;
}

static int serial_parse_stopbits(Jim_Interp *interp, const char *str, uart_stop_bits_t *bits)
{
    if (strcmp(str, "1") == 0)        *bits = UART_STOP_BITS_1;
    else if (strcmp(str, "1.5") == 0) *bits = UART_STOP_BITS_1_5;
    else if (strcmp(str, "2") == 0)   *bits = UART_STOP_BITS_2;
    else {
        Jim_SetResultFormatted(interp, "bad stopbits \"%s\": should be 1, 1.5, or 2", str);
        return JIM_ERR;
    }
    return JIM_OK;
}

static int serial_parse_parity(Jim_Interp *interp, const char *str, uart_parity_t *parity)
{
    if (strcmp(str, "none") == 0)      *parity = UART_PARITY_DISABLE;
    else if (strcmp(str, "even") == 0) *parity = UART_PARITY_EVEN;
    else if (strcmp(str, "odd") == 0)  *parity = UART_PARITY_ODD;
    else {
        Jim_SetResultFormatted(interp, "bad parity \"%s\": should be none, even, or odd", str);
        return JIM_ERR;
    }
    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Listener task — routes incoming UART data to a Tcl task
 * ---------------------------------------------------------------------------*/

typedef struct {
    int port;
} serial_listener_ctx_t;

static void serial_listener_fn(void *param)
{
    serial_listener_ctx_t *ctx = (serial_listener_ctx_t *)param;
    int port = ctx->port;
    free(ctx);

    serial_port_state_t *state = &serial_ports[port];
    uint8_t buf[SERIAL_READ_CHUNK];

    while (!state->listener_stop) {
        int len = uart_read_bytes((uart_port_t)port, buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (len <= 0) continue;

        /* Deliver data as string to the target task */
        /* Build: {proc} {data} */
        char *script = malloc(strlen(state->listener_proc) + len * 4 + 16);
        if (!script) continue;

        /* Format data as the raw string, braces-quoted for safety */
        int off = snprintf(script, strlen(state->listener_proc) + 4, "%s {",
                           state->listener_proc);
        for (int i = 0; i < len; i++) {
            if (buf[i] >= 0x20 && buf[i] < 0x7f && buf[i] != '{' && buf[i] != '}' && buf[i] != '\\') {
                script[off++] = (char)buf[i];
            } else {
                off += snprintf(script + off, 8, "\\x%02x", buf[i]);
            }
        }
        script[off++] = '}';
        script[off] = '\0';

        if (task_send_to_name(state->listener_target, script) != 0) {
            ESP_LOGW(TAG, "Serial listener delivery failed: port %d -> task '%s'",
                     port, state->listener_target);
        }
        free(script);
    }

    ESP_LOGI(TAG, "Serial listener stopped on port %d", port);
    state->listener_task = NULL;
    vTaskDelete(NULL);
}

/* ---------------------------------------------------------------------------
 * Subcommand: serial open <port> -tx <pin> -rx <pin> -baud <rate> ...
 * ---------------------------------------------------------------------------*/

static int serial_cmd_open(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc < 1) {
        Jim_SetResultString(interp,
            "wrong # args: should be \"serial open port -tx pin -rx pin -baud rate ...\"", -1);
        return JIM_ERR;
    }

    int port;
    if (serial_get_port(interp, argv[0], &port) != JIM_OK) return JIM_ERR;

    if (port == 0) {
        ESP_LOGW(TAG, "Warning: UART0 is typically used for console output");
    }

    long tx_pin = -1, rx_pin = -1, baud = 115200;
    uart_word_length_t databits = UART_DATA_8_BITS;
    uart_stop_bits_t stopbits = UART_STOP_BITS_1;
    uart_parity_t parity = UART_PARITY_DISABLE;

    for (int i = 1; i < argc; i++) {
        const char *opt = Jim_String(argv[i]);
        if (strcmp(opt, "-tx") == 0 && i + 1 < argc) {
            if (Jim_GetLong(interp, argv[++i], &tx_pin) != JIM_OK) return JIM_ERR;
        } else if (strcmp(opt, "-rx") == 0 && i + 1 < argc) {
            if (Jim_GetLong(interp, argv[++i], &rx_pin) != JIM_OK) return JIM_ERR;
        } else if (strcmp(opt, "-baud") == 0 && i + 1 < argc) {
            if (Jim_GetLong(interp, argv[++i], &baud) != JIM_OK) return JIM_ERR;
        } else if (strcmp(opt, "-databits") == 0 && i + 1 < argc) {
            if (serial_parse_databits(interp, Jim_String(argv[++i]), &databits) != JIM_OK)
                return JIM_ERR;
        } else if (strcmp(opt, "-stopbits") == 0 && i + 1 < argc) {
            if (serial_parse_stopbits(interp, Jim_String(argv[++i]), &stopbits) != JIM_OK)
                return JIM_ERR;
        } else if (strcmp(opt, "-parity") == 0 && i + 1 < argc) {
            if (serial_parse_parity(interp, Jim_String(argv[++i]), &parity) != JIM_OK)
                return JIM_ERR;
        } else {
            Jim_SetResultFormatted(interp, "unknown option \"%s\"", opt);
            return JIM_ERR;
        }
    }

    if (tx_pin < 0 || rx_pin < 0) {
        Jim_SetResultString(interp,
            "must specify both -tx and -rx pins", -1);
        return JIM_ERR;
    }

    /* Close existing driver on this port if already open */
    if (serial_ports[port].installed) {
        /* Stop listener if running */
        if (serial_ports[port].listener_task) {
            serial_ports[port].listener_stop = 1;
            int wait = 0;
            while (serial_ports[port].listener_task && wait < 20) {
                vTaskDelay(pdMS_TO_TICKS(50));
                wait++;
            }
            if (serial_ports[port].listener_task) {
                vTaskDelete(serial_ports[port].listener_task);
                serial_ports[port].listener_task = NULL;
            }
            serial_ports[port].listener_stop = 0;
        }
        uart_driver_delete((uart_port_t)port);
        serial_ports[port].installed = 0;
    }

    /* Configure UART */
    uart_config_t uart_config = {
        .baud_rate = (int)baud,
        .data_bits = databits,
        .parity = parity,
        .stop_bits = stopbits,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_param_config((uart_port_t)port, &uart_config);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "uart_param_config failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    err = uart_set_pin((uart_port_t)port, (int)tx_pin, (int)rx_pin,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "uart_set_pin failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    err = uart_driver_install((uart_port_t)port, SERIAL_RX_BUF, SERIAL_TX_BUF, 0, NULL, 0);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "uart_driver_install failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    serial_ports[port].installed = 1;
    ESP_LOGI(TAG, "UART%d opened: TX=%ld RX=%ld baud=%ld", port, tx_pin, rx_pin, baud);
    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Subcommand: serial write <port> <data>
 * ---------------------------------------------------------------------------*/

static int serial_cmd_write(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc < 2) {
        Jim_SetResultString(interp,
            "wrong # args: should be \"serial write port data\"", -1);
        return JIM_ERR;
    }

    int port;
    if (serial_get_port(interp, argv[0], &port) != JIM_OK) return JIM_ERR;

    if (!serial_ports[port].installed) {
        Jim_SetResultFormatted(interp, "UART%d not open", port);
        return JIM_ERR;
    }

    int data_len;
    const char *data = Jim_GetString(argv[1], &data_len);

    int written = uart_write_bytes((uart_port_t)port, data, data_len);
    if (written < 0) {
        Jim_SetResultString(interp, "uart_write_bytes failed", -1);
        return JIM_ERR;
    }

    Jim_SetResultInt(interp, written);
    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Subcommand: serial read <port> ?length? ?-timeout ms?
 * ---------------------------------------------------------------------------*/

static int serial_cmd_read(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc < 1) {
        Jim_SetResultString(interp,
            "wrong # args: should be \"serial read port ?length? ?-timeout ms?\"", -1);
        return JIM_ERR;
    }

    int port;
    if (serial_get_port(interp, argv[0], &port) != JIM_OK) return JIM_ERR;

    if (!serial_ports[port].installed) {
        Jim_SetResultFormatted(interp, "UART%d not open", port);
        return JIM_ERR;
    }

    long length = SERIAL_READ_CHUNK;
    long timeout_ms = 1000;

    for (int i = 1; i < argc; i++) {
        const char *opt = Jim_String(argv[i]);
        if (strcmp(opt, "-timeout") == 0 && i + 1 < argc) {
            if (Jim_GetLong(interp, argv[++i], &timeout_ms) != JIM_OK) return JIM_ERR;
        } else {
            /* Assume it's the length argument */
            if (Jim_GetLong(interp, argv[i], &length) != JIM_OK) return JIM_ERR;
            if (length <= 0 || length > 65536) {
                Jim_SetResultFormatted(interp, "invalid read length: %ld", length);
                return JIM_ERR;
            }
        }
    }

    uint8_t *buf = malloc(length);
    if (!buf) {
        Jim_SetResultString(interp, "out of memory", -1);
        return JIM_ERR;
    }

    int len = uart_read_bytes((uart_port_t)port, buf, length, pdMS_TO_TICKS(timeout_ms));
    if (len < 0) {
        free(buf);
        Jim_SetResultString(interp, "uart_read_bytes failed", -1);
        return JIM_ERR;
    }

    Jim_SetResultString(interp, (const char *)buf, len);
    free(buf);
    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Subcommand: serial readline <port> ?-timeout ms?
 * ---------------------------------------------------------------------------*/

static int serial_cmd_readline(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc < 1) {
        Jim_SetResultString(interp,
            "wrong # args: should be \"serial readline port ?-timeout ms?\"", -1);
        return JIM_ERR;
    }

    int port;
    if (serial_get_port(interp, argv[0], &port) != JIM_OK) return JIM_ERR;

    if (!serial_ports[port].installed) {
        Jim_SetResultFormatted(interp, "UART%d not open", port);
        return JIM_ERR;
    }

    long timeout_ms = 1000;
    for (int i = 1; i < argc; i++) {
        const char *opt = Jim_String(argv[i]);
        if (strcmp(opt, "-timeout") == 0 && i + 1 < argc) {
            if (Jim_GetLong(interp, argv[++i], &timeout_ms) != JIM_OK) return JIM_ERR;
        } else {
            Jim_SetResultFormatted(interp, "unknown option \"%s\"", opt);
            return JIM_ERR;
        }
    }

    /* Read byte-by-byte until newline or timeout */
    char line[1024];
    int pos = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (pos < (int)sizeof(line) - 1) {
        TickType_t remaining = deadline - xTaskGetTickCount();
        if ((int32_t)remaining <= 0) break;

        uint8_t ch;
        int len = uart_read_bytes((uart_port_t)port, &ch, 1, remaining);
        if (len <= 0) break;

        if (ch == '\n') {
            break;
        }
        if (ch == '\r') {
            continue;  /* Skip carriage returns */
        }
        line[pos++] = (char)ch;
    }

    Jim_SetResultString(interp, line, pos);
    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Subcommand: serial available <port>
 * ---------------------------------------------------------------------------*/

static int serial_cmd_available(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc < 1) {
        Jim_SetResultString(interp,
            "wrong # args: should be \"serial available port\"", -1);
        return JIM_ERR;
    }

    int port;
    if (serial_get_port(interp, argv[0], &port) != JIM_OK) return JIM_ERR;

    if (!serial_ports[port].installed) {
        Jim_SetResultFormatted(interp, "UART%d not open", port);
        return JIM_ERR;
    }

    size_t buffered = 0;
    esp_err_t err = uart_get_buffered_data_len((uart_port_t)port, &buffered);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "uart_get_buffered_data_len failed: %s",
                               esp_err_to_name(err));
        return JIM_ERR;
    }

    Jim_SetResultInt(interp, (jim_wide)buffered);
    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Subcommand: serial listen <port> -callback {proc task} | -remove
 * ---------------------------------------------------------------------------*/

static int serial_cmd_listen(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc < 2) {
        Jim_SetResultString(interp,
            "wrong # args: should be \"serial listen port -callback {proc task}\" or "
            "\"serial listen port -remove\"", -1);
        return JIM_ERR;
    }

    int port;
    if (serial_get_port(interp, argv[0], &port) != JIM_OK) return JIM_ERR;

    if (!serial_ports[port].installed) {
        Jim_SetResultFormatted(interp, "UART%d not open", port);
        return JIM_ERR;
    }

    serial_port_state_t *state = &serial_ports[port];
    const char *opt = Jim_String(argv[1]);

    /* serial listen <port> -remove */
    if (strcmp(opt, "-remove") == 0) {
        if (state->listener_task) {
            state->listener_stop = 1;
            int wait = 0;
            while (state->listener_task && wait < 20) {
                vTaskDelay(pdMS_TO_TICKS(50));
                wait++;
            }
            if (state->listener_task) {
                ESP_LOGW(TAG, "Serial listener on port %d did not stop cleanly", port);
                vTaskDelete(state->listener_task);
                state->listener_task = NULL;
            }
            state->listener_stop = 0;
            ESP_LOGI(TAG, "Serial listener removed from port %d", port);
        }
        return JIM_OK;
    }

    /* serial listen <port> -callback {proc task} */
    if (strcmp(opt, "-callback") == 0 && argc >= 3) {
        Jim_Obj *cbObj = argv[2];
        if (Jim_ListLength(interp, cbObj) != 2) {
            Jim_SetResultString(interp, "-callback requires {procname target_task}", -1);
            return JIM_ERR;
        }

        const char *proc = Jim_String(Jim_ListGetIndex(interp, cbObj, 0));
        const char *target = Jim_String(Jim_ListGetIndex(interp, cbObj, 1));

        /* Stop existing listener if any */
        if (state->listener_task) {
            state->listener_stop = 1;
            int wait = 0;
            while (state->listener_task && wait < 20) {
                vTaskDelay(pdMS_TO_TICKS(50));
                wait++;
            }
            if (state->listener_task) {
                vTaskDelete(state->listener_task);
                state->listener_task = NULL;
            }
            state->listener_stop = 0;
        }

        strncpy(state->listener_proc, proc, sizeof(state->listener_proc) - 1);
        state->listener_proc[sizeof(state->listener_proc) - 1] = '\0';
        strncpy(state->listener_target, target, sizeof(state->listener_target) - 1);
        state->listener_target[sizeof(state->listener_target) - 1] = '\0';

        serial_listener_ctx_t *ctx = malloc(sizeof(*ctx));
        if (!ctx) {
            Jim_SetResultString(interp, "out of memory", -1);
            return JIM_ERR;
        }
        ctx->port = port;

        char task_name[16];
        snprintf(task_name, sizeof(task_name), "uart%d_listen", port);

        BaseType_t ret = xTaskCreate(serial_listener_fn, task_name, 4096, ctx, 6,
                                     &state->listener_task);
        if (ret != pdPASS) {
            free(ctx);
            state->listener_task = NULL;
            Jim_SetResultString(interp, "failed to create serial listener task", -1);
            return JIM_ERR;
        }

        ESP_LOGI(TAG, "Serial listener started on port %d: %s -> task '%s'",
                 port, proc, target);
        return JIM_OK;
    }

    Jim_SetResultFormatted(interp, "unknown option \"%s\"", opt);
    return JIM_ERR;
}

/* ---------------------------------------------------------------------------
 * Subcommand: serial close <port>
 * ---------------------------------------------------------------------------*/

static int serial_cmd_close(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc < 1) {
        Jim_SetResultString(interp,
            "wrong # args: should be \"serial close port\"", -1);
        return JIM_ERR;
    }

    int port;
    if (serial_get_port(interp, argv[0], &port) != JIM_OK) return JIM_ERR;

    if (!serial_ports[port].installed) {
        Jim_SetResultFormatted(interp, "UART%d not open", port);
        return JIM_ERR;
    }

    serial_port_state_t *state = &serial_ports[port];

    /* Stop listener if running */
    if (state->listener_task) {
        state->listener_stop = 1;
        int wait = 0;
        while (state->listener_task && wait < 20) {
            vTaskDelay(pdMS_TO_TICKS(50));
            wait++;
        }
        if (state->listener_task) {
            vTaskDelete(state->listener_task);
            state->listener_task = NULL;
        }
        state->listener_stop = 0;
    }

    esp_err_t err = uart_driver_delete((uart_port_t)port);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "uart_driver_delete failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    state->installed = 0;
    ESP_LOGI(TAG, "UART%d closed", port);
    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Subcommand table and init
 * ---------------------------------------------------------------------------*/

static const jim_subcmd_type serial_command_table[] = {
    {   "open",
        "port -tx pin -rx pin -baud rate ?-databits 5|6|7|8? ?-stopbits 1|1.5|2? ?-parity none|even|odd?",
        serial_cmd_open,
        1,
        -1,
        /* Description: Open a UART port with configuration */
    },
    {   "write",
        "port data",
        serial_cmd_write,
        2,
        2,
        /* Description: Write data to UART port */
    },
    {   "read",
        "port ?length? ?-timeout ms?",
        serial_cmd_read,
        1,
        -1,
        /* Description: Read data from UART port */
    },
    {   "readline",
        "port ?-timeout ms?",
        serial_cmd_readline,
        1,
        -1,
        /* Description: Read a line from UART port */
    },
    {   "available",
        "port",
        serial_cmd_available,
        1,
        1,
        /* Description: Get bytes available in RX buffer */
    },
    {   "listen",
        "port -callback {proc task} | port -remove",
        serial_cmd_listen,
        2,
        -1,
        /* Description: Async listener for incoming UART data */
    },
    {   "close",
        "port",
        serial_cmd_close,
        1,
        1,
        /* Description: Close UART port */
    },
    { NULL }
};

int Jim_serialInit(Jim_Interp *interp)
{
    Jim_PackageProvideCheck(interp, "serial");
    Jim_RegisterSubCmd(interp, "serial", serial_command_table, NULL);
    return JIM_OK;
}
