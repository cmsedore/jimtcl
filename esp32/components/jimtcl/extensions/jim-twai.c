/* Jim Tcl TWAI (CAN bus) Extension for ESP32
 *
 * Provides Tcl commands for TWAI/CAN bus communication:
 *
 *   twai init -tx <pin> -rx <pin> -speed 25k|50k|100k|125k|250k|500k|800k|1M
 *   twai start
 *   twai stop
 *   twai send <id> <data_bytes> ?-extended? ?-rtr?
 *   twai receive ?timeout_ms?
 *   twai listen -callback {proc task}
 *   twai listen -remove
 *   twai filter <id> <mask>
 *   twai status
 */

#include <string.h>
#include <stdlib.h>
#include "jim.h"
#include "jim-subcmd.h"
#include "jim-esp32-task.h"
#include "soc/soc_caps.h"

#if defined(SOC_TWAI_SUPPORTED) && SOC_TWAI_SUPPORTED

#include "driver/twai.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "jim-twai";

/* ---------------------------------------------------------------------------
 * Static state
 * ---------------------------------------------------------------------------*/

static int twai_driver_installed = 0;
static int twai_driver_started = 0;

/* Stored filter for reinit */
static twai_filter_config_t twai_current_filter = TWAI_FILTER_CONFIG_ACCEPT_ALL();

/* Listener task state */
static TaskHandle_t twai_listener_task = NULL;
static volatile int twai_listener_stop = 0;
static char twai_listener_proc[64] = {0};
static char twai_listener_target[16] = {0};

/* ---------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------------*/

static int twai_parse_speed(Jim_Interp *interp, const char *speed_str,
                            twai_timing_config_t *timing)
{
    if (strcmp(speed_str, "25k") == 0) {
        twai_timing_config_t t = TWAI_TIMING_CONFIG_25KBITS();
        *timing = t;
    } else if (strcmp(speed_str, "50k") == 0) {
        twai_timing_config_t t = TWAI_TIMING_CONFIG_50KBITS();
        *timing = t;
    } else if (strcmp(speed_str, "100k") == 0) {
        twai_timing_config_t t = TWAI_TIMING_CONFIG_100KBITS();
        *timing = t;
    } else if (strcmp(speed_str, "125k") == 0) {
        twai_timing_config_t t = TWAI_TIMING_CONFIG_125KBITS();
        *timing = t;
    } else if (strcmp(speed_str, "250k") == 0) {
        twai_timing_config_t t = TWAI_TIMING_CONFIG_250KBITS();
        *timing = t;
    } else if (strcmp(speed_str, "500k") == 0) {
        twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
        *timing = t;
    } else if (strcmp(speed_str, "800k") == 0) {
        twai_timing_config_t t = TWAI_TIMING_CONFIG_800KBITS();
        *timing = t;
    } else if (strcmp(speed_str, "1M") == 0) {
        twai_timing_config_t t = TWAI_TIMING_CONFIG_1MBITS();
        *timing = t;
    } else {
        Jim_SetResultFormatted(interp,
            "bad speed \"%s\": should be 25k, 50k, 100k, 125k, 250k, 500k, 800k, or 1M",
            speed_str);
        return JIM_ERR;
    }
    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Listener task — routes incoming CAN frames to a Tcl task
 * ---------------------------------------------------------------------------*/

static void twai_listener_fn(void *param)
{
    twai_message_t msg;
    while (!twai_listener_stop) {
        esp_err_t err = twai_receive(&msg, pdMS_TO_TICKS(100));
        if (err == ESP_ERR_TIMEOUT) {
            continue;
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "twai_receive error in listener: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* Build data list as space-separated hex bytes */
        char data_list[64] = {0};
        int off = 0;
        for (int i = 0; i < msg.data_length_code && i < 8; i++) {
            if (i > 0) data_list[off++] = ' ';
            off += snprintf(data_list + off, sizeof(data_list) - off, "0x%02x", msg.data[i]);
        }

        /* Build flags dict */
        char flags[64];
        snprintf(flags, sizeof(flags), "extended %d rtr %d",
                 msg.extd ? 1 : 0, msg.rtr ? 1 : 0);

        /* Script: {proc} {id} {data_list} {dlc} {flags} */
        char script[256];
        snprintf(script, sizeof(script), "%s 0x%lx {%s} %d {%s}",
                 twai_listener_proc,
                 (unsigned long)msg.identifier,
                 data_list,
                 (int)msg.data_length_code,
                 flags);

        if (task_send_to_name(twai_listener_target, script) != 0) {
            ESP_LOGW(TAG, "TWAI listener delivery failed -> task '%s'",
                     twai_listener_target);
        }
    }

    ESP_LOGI(TAG, "TWAI listener task stopped");
    twai_listener_task = NULL;
    vTaskDelete(NULL);
}

/* ---------------------------------------------------------------------------
 * Subcommand: twai init -tx <pin> -rx <pin> -speed <rate>
 * ---------------------------------------------------------------------------*/

static int twai_cmd_init(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    long tx_pin = -1, rx_pin = -1;
    const char *speed_str = NULL;

    for (int i = 0; i < argc; i++) {
        const char *opt = Jim_String(argv[i]);
        if (strcmp(opt, "-tx") == 0 && i + 1 < argc) {
            if (Jim_GetLong(interp, argv[++i], &tx_pin) != JIM_OK) return JIM_ERR;
        } else if (strcmp(opt, "-rx") == 0 && i + 1 < argc) {
            if (Jim_GetLong(interp, argv[++i], &rx_pin) != JIM_OK) return JIM_ERR;
        } else if (strcmp(opt, "-speed") == 0 && i + 1 < argc) {
            speed_str = Jim_String(argv[++i]);
        } else {
            Jim_SetResultFormatted(interp, "unknown option \"%s\"", opt);
            return JIM_ERR;
        }
    }

    if (tx_pin < 0 || rx_pin < 0 || !speed_str) {
        Jim_SetResultString(interp,
            "usage: twai init -tx <pin> -rx <pin> -speed <rate>", -1);
        return JIM_ERR;
    }

    /* Uninstall existing driver if re-initializing */
    if (twai_driver_installed) {
        if (twai_driver_started) {
            twai_stop();
            twai_driver_started = 0;
        }
        twai_driver_uninstall();
        twai_driver_installed = 0;
    }

    twai_timing_config_t timing;
    if (twai_parse_speed(interp, speed_str, &timing) != JIM_OK) return JIM_ERR;

    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(
        (gpio_num_t)tx_pin, (gpio_num_t)rx_pin, TWAI_MODE_NORMAL);

    esp_err_t err = twai_driver_install(&g_config, &timing, &twai_current_filter);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "twai_driver_install failed: %s",
                               esp_err_to_name(err));
        return JIM_ERR;
    }

    twai_driver_installed = 1;
    ESP_LOGI(TAG, "TWAI initialized: TX=%ld RX=%ld speed=%s", tx_pin, rx_pin, speed_str);
    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Subcommand: twai start
 * ---------------------------------------------------------------------------*/

static int twai_cmd_start(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (!twai_driver_installed) {
        Jim_SetResultString(interp, "TWAI driver not initialized (call twai init first)", -1);
        return JIM_ERR;
    }

    esp_err_t err = twai_start();
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "twai_start failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    twai_driver_started = 1;
    ESP_LOGI(TAG, "TWAI started");
    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Subcommand: twai stop
 * ---------------------------------------------------------------------------*/

static int twai_cmd_stop(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (!twai_driver_started) {
        Jim_SetResultString(interp, "TWAI not running", -1);
        return JIM_ERR;
    }

    esp_err_t err = twai_stop();
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "twai_stop failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    twai_driver_started = 0;
    ESP_LOGI(TAG, "TWAI stopped");
    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Subcommand: twai send <id> <data_bytes> ?-extended? ?-rtr?
 * ---------------------------------------------------------------------------*/

static int twai_cmd_send(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (!twai_driver_started) {
        Jim_SetResultString(interp, "TWAI not running (call twai start first)", -1);
        return JIM_ERR;
    }

    if (argc < 2) {
        Jim_SetResultString(interp,
            "wrong # args: should be \"twai send id data_bytes ?-extended? ?-rtr?\"", -1);
        return JIM_ERR;
    }

    /* Parse message ID */
    long msg_id;
    if (Jim_GetLong(interp, argv[0], &msg_id) != JIM_OK) return JIM_ERR;

    /* Parse data bytes from list */
    int data_len = Jim_ListLength(interp, argv[1]);
    if (data_len > 8) {
        Jim_SetResultString(interp, "CAN data cannot exceed 8 bytes", -1);
        return JIM_ERR;
    }

    twai_message_t msg = {0};
    msg.identifier = (uint32_t)msg_id;
    msg.data_length_code = (uint8_t)data_len;

    for (int i = 0; i < data_len; i++) {
        long byte_val;
        Jim_Obj *elem = Jim_ListGetIndex(interp, argv[1], i);
        if (Jim_GetLong(interp, elem, &byte_val) != JIM_OK) return JIM_ERR;
        if (byte_val < 0 || byte_val > 255) {
            Jim_SetResultFormatted(interp, "data byte out of range: %ld", byte_val);
            return JIM_ERR;
        }
        msg.data[i] = (uint8_t)byte_val;
    }

    /* Parse optional flags */
    for (int i = 2; i < argc; i++) {
        const char *opt = Jim_String(argv[i]);
        if (strcmp(opt, "-extended") == 0) {
            msg.extd = 1;
        } else if (strcmp(opt, "-rtr") == 0) {
            msg.rtr = 1;
        } else {
            Jim_SetResultFormatted(interp, "unknown option \"%s\"", opt);
            return JIM_ERR;
        }
    }

    esp_err_t err = twai_transmit(&msg, pdMS_TO_TICKS(1000));
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "twai_transmit failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    ESP_LOGD(TAG, "TWAI sent: id=0x%lx dlc=%d", (unsigned long)msg.identifier, msg.data_length_code);
    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Subcommand: twai receive ?timeout_ms?
 * ---------------------------------------------------------------------------*/

static int twai_cmd_receive(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (!twai_driver_started) {
        Jim_SetResultString(interp, "TWAI not running (call twai start first)", -1);
        return JIM_ERR;
    }

    long timeout_ms = 1000;
    if (argc >= 1) {
        if (Jim_GetLong(interp, argv[0], &timeout_ms) != JIM_OK) return JIM_ERR;
    }

    twai_message_t msg;
    esp_err_t err = twai_receive(&msg, pdMS_TO_TICKS(timeout_ms));
    if (err == ESP_ERR_TIMEOUT) {
        Jim_SetResultString(interp, "", -1);
        return JIM_OK;
    }
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "twai_receive failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    /* Build result dict: {id <hex_id> data {byte list} dlc <n> extended <0|1> rtr <0|1>} */
    Jim_Obj *result = Jim_NewListObj(interp, NULL, 0);

    /* id */
    char id_str[16];
    snprintf(id_str, sizeof(id_str), "0x%lx", (unsigned long)msg.identifier);
    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "id", -1));
    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, id_str, -1));

    /* data */
    Jim_Obj *data_list = Jim_NewListObj(interp, NULL, 0);
    for (int i = 0; i < msg.data_length_code && i < 8; i++) {
        char byte_str[8];
        snprintf(byte_str, sizeof(byte_str), "0x%02x", msg.data[i]);
        Jim_ListAppendElement(interp, data_list, Jim_NewStringObj(interp, byte_str, -1));
    }
    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "data", -1));
    Jim_ListAppendElement(interp, result, data_list);

    /* dlc */
    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "dlc", -1));
    Jim_ListAppendElement(interp, result, Jim_NewIntObj(interp, msg.data_length_code));

    /* extended */
    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "extended", -1));
    Jim_ListAppendElement(interp, result, Jim_NewIntObj(interp, msg.extd ? 1 : 0));

    /* rtr */
    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "rtr", -1));
    Jim_ListAppendElement(interp, result, Jim_NewIntObj(interp, msg.rtr ? 1 : 0));

    Jim_SetResult(interp, result);
    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Subcommand: twai listen -callback {proc task} | -remove
 * ---------------------------------------------------------------------------*/

static int twai_cmd_listen(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (!twai_driver_started) {
        Jim_SetResultString(interp, "TWAI not running (call twai start first)", -1);
        return JIM_ERR;
    }

    if (argc < 1) {
        Jim_SetResultString(interp,
            "wrong # args: should be \"twai listen -callback {proc task}\" or "
            "\"twai listen -remove\"", -1);
        return JIM_ERR;
    }

    const char *opt = Jim_String(argv[0]);

    /* twai listen -remove */
    if (strcmp(opt, "-remove") == 0) {
        if (twai_listener_task) {
            twai_listener_stop = 1;
            /* Wait for the task to exit (it checks every 100ms) */
            int wait = 0;
            while (twai_listener_task && wait < 20) {
                vTaskDelay(pdMS_TO_TICKS(50));
                wait++;
            }
            if (twai_listener_task) {
                ESP_LOGW(TAG, "TWAI listener task did not stop cleanly");
                vTaskDelete(twai_listener_task);
                twai_listener_task = NULL;
            }
            twai_listener_stop = 0;
            ESP_LOGI(TAG, "TWAI listener removed");
        }
        return JIM_OK;
    }

    /* twai listen -callback {proc task} */
    if (strcmp(opt, "-callback") == 0 && argc >= 2) {
        Jim_Obj *cbObj = argv[1];
        if (Jim_ListLength(interp, cbObj) != 2) {
            Jim_SetResultString(interp, "-callback requires {procname target_task}", -1);
            return JIM_ERR;
        }

        const char *proc = Jim_String(Jim_ListGetIndex(interp, cbObj, 0));
        const char *target = Jim_String(Jim_ListGetIndex(interp, cbObj, 1));

        /* Stop existing listener if any */
        if (twai_listener_task) {
            twai_listener_stop = 1;
            int wait = 0;
            while (twai_listener_task && wait < 20) {
                vTaskDelay(pdMS_TO_TICKS(50));
                wait++;
            }
            if (twai_listener_task) {
                vTaskDelete(twai_listener_task);
                twai_listener_task = NULL;
            }
            twai_listener_stop = 0;
        }

        strncpy(twai_listener_proc, proc, sizeof(twai_listener_proc) - 1);
        twai_listener_proc[sizeof(twai_listener_proc) - 1] = '\0';
        strncpy(twai_listener_target, target, sizeof(twai_listener_target) - 1);
        twai_listener_target[sizeof(twai_listener_target) - 1] = '\0';

        BaseType_t ret = xTaskCreate(twai_listener_fn, "twai_listen", 4096, NULL, 6,
                                     &twai_listener_task);
        if (ret != pdPASS) {
            twai_listener_task = NULL;
            Jim_SetResultString(interp, "failed to create TWAI listener task", -1);
            return JIM_ERR;
        }

        ESP_LOGI(TAG, "TWAI listener started: %s -> task '%s'", proc, target);
        return JIM_OK;
    }

    Jim_SetResultFormatted(interp, "unknown option \"%s\"", opt);
    return JIM_ERR;
}

/* ---------------------------------------------------------------------------
 * Subcommand: twai filter <id> <mask>
 * ---------------------------------------------------------------------------*/

static int twai_cmd_filter(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc < 2) {
        Jim_SetResultString(interp,
            "wrong # args: should be \"twai filter id mask\"", -1);
        return JIM_ERR;
    }

    long filter_id, filter_mask;
    if (Jim_GetLong(interp, argv[0], &filter_id) != JIM_OK) return JIM_ERR;
    if (Jim_GetLong(interp, argv[1], &filter_mask) != JIM_OK) return JIM_ERR;

    twai_current_filter.acceptance_code = (uint32_t)filter_id;
    twai_current_filter.acceptance_mask = (uint32_t)filter_mask;
    twai_current_filter.single_filter = true;

    ESP_LOGI(TAG, "TWAI filter set: code=0x%lx mask=0x%lx (applies on next init)",
             (unsigned long)filter_id, (unsigned long)filter_mask);

    /* If driver is installed, we need to reinstall to apply the filter */
    if (twai_driver_installed) {
        Jim_SetResultString(interp,
            "filter stored; call twai init again to apply, or it will apply on next init",
            -1);
    }

    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Subcommand: twai status
 * ---------------------------------------------------------------------------*/

static int twai_cmd_status(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (!twai_driver_installed) {
        Jim_SetResultString(interp, "TWAI driver not initialized", -1);
        return JIM_ERR;
    }

    twai_status_info_t status;
    esp_err_t err = twai_get_status_info(&status);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "twai_get_status_info failed: %s",
                               esp_err_to_name(err));
        return JIM_ERR;
    }

    /* Map state enum to string */
    const char *state_str;
    switch (status.state) {
        case TWAI_STATE_STOPPED:   state_str = "stopped"; break;
        case TWAI_STATE_RUNNING:   state_str = "running"; break;
        case TWAI_STATE_BUS_OFF:   state_str = "bus_off"; break;
        case TWAI_STATE_RECOVERING: state_str = "recovering"; break;
        default: state_str = "unknown"; break;
    }

    Jim_Obj *result = Jim_NewListObj(interp, NULL, 0);

    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "state", -1));
    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, state_str, -1));

    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "tx_error_counter", -1));
    Jim_ListAppendElement(interp, result, Jim_NewIntObj(interp, status.tx_error_counter));

    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "rx_error_counter", -1));
    Jim_ListAppendElement(interp, result, Jim_NewIntObj(interp, status.rx_error_counter));

    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "msgs_to_tx", -1));
    Jim_ListAppendElement(interp, result, Jim_NewIntObj(interp, status.msgs_to_tx));

    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "msgs_to_rx", -1));
    Jim_ListAppendElement(interp, result, Jim_NewIntObj(interp, status.msgs_to_rx));

    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "tx_failed_count", -1));
    Jim_ListAppendElement(interp, result, Jim_NewIntObj(interp, status.tx_failed_count));

    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "rx_missed_count", -1));
    Jim_ListAppendElement(interp, result, Jim_NewIntObj(interp, status.rx_missed_count));

    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "arb_lost_count", -1));
    Jim_ListAppendElement(interp, result, Jim_NewIntObj(interp, status.arb_lost_count));

    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "bus_error_count", -1));
    Jim_ListAppendElement(interp, result, Jim_NewIntObj(interp, status.bus_error_count));

    Jim_SetResult(interp, result);
    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Subcommand table and init
 * ---------------------------------------------------------------------------*/

static const jim_subcmd_type twai_command_table[] = {
    {   "init",
        "-tx pin -rx pin -speed 25k|50k|100k|125k|250k|500k|800k|1M",
        twai_cmd_init,
        6,
        6,
        /* Description: Initialize TWAI (CAN bus) driver */
    },
    {   "start",
        "",
        twai_cmd_start,
        0,
        0,
        /* Description: Start TWAI driver */
    },
    {   "stop",
        "",
        twai_cmd_stop,
        0,
        0,
        /* Description: Stop TWAI driver */
    },
    {   "send",
        "id data_bytes ?-extended? ?-rtr?",
        twai_cmd_send,
        2,
        -1,
        /* Description: Transmit a CAN frame */
    },
    {   "receive",
        "?timeout_ms?",
        twai_cmd_receive,
        0,
        1,
        /* Description: Receive a CAN frame (synchronous) */
    },
    {   "listen",
        "-callback {proc task} | -remove",
        twai_cmd_listen,
        1,
        -1,
        /* Description: Async listener for incoming CAN frames */
    },
    {   "filter",
        "id mask",
        twai_cmd_filter,
        2,
        2,
        /* Description: Set acceptance filter for incoming frames */
    },
    {   "status",
        "",
        twai_cmd_status,
        0,
        0,
        /* Description: Get TWAI bus status and error counters */
    },
    { NULL }
};

int Jim_twaiInit(Jim_Interp *interp)
{
    Jim_PackageProvideCheck(interp, "twai");
    Jim_RegisterSubCmd(interp, "twai", twai_command_table, NULL);
    return JIM_OK;
}

#endif /* SOC_TWAI_SUPPORTED */
