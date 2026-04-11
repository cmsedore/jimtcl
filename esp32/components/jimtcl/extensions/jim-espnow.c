/* Jim Tcl ESP-NOW Extension for ESP32
 *
 * Provides Tcl commands for ESP-NOW peer-to-peer communication:
 *
 *   espnow init ?-pmk <16-byte-key>?
 *   espnow deinit
 *   espnow peer add <mac_addr> ?-channel ch? ?-encrypt 0|1? ?-key <16-byte-key>?
 *   espnow peer remove <mac_addr>
 *   espnow peer list
 *   espnow send <mac_addr> <data> ?-mpack?
 *   espnow receive ?timeout_ms? ?-mpack?
 *   espnow listen -callback {proc task} ?-mpack?
 *   espnow listen -remove
 *   espnow status
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "jim.h"
#include "jim-subcmd.h"
#include "jim-esp32-task.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#ifdef CONFIG_JIM_EXT_MPACK
#include "jim-mpack.h"
#endif

static const char *TAG = "jim-espnow";

/* ---------------------------------------------------------------------------
 * Static state
 * ---------------------------------------------------------------------------*/

#define ESPNOW_MAX_PAYLOAD  250
#define ESPNOW_RX_QUEUE_LEN 8

typedef struct {
    uint8_t mac[6];
    uint8_t data[ESPNOW_MAX_PAYLOAD];
    int len;
} espnow_rx_item_t;

static struct {
    int initialized;
    QueueHandle_t rx_queue;
    /* Listener state */
    TaskHandle_t listener_task;
    volatile int listener_stop;
    char listener_proc[64];
    char listener_target[16];
    int listener_mpack;
} espnow_state = {0};

/* ---------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------------*/

static int parse_mac(const char *str, uint8_t mac[6])
{
    unsigned int m[6];
    if (sscanf(str, "%x:%x:%x:%x:%x:%x",
               &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6) {
        return -1;
    }
    for (int i = 0; i < 6; i++) {
        if (m[i] > 0xFF) return -1;
        mac[i] = (uint8_t)m[i];
    }
    return 0;
}

static void format_mac(const uint8_t mac[6], char *buf, size_t buflen)
{
    snprintf(buf, buflen, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* ---------------------------------------------------------------------------
 * ESP-NOW callbacks
 * ---------------------------------------------------------------------------*/

static void espnow_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    /* Could track send success/failure if needed */
    if (status != ESP_NOW_SEND_SUCCESS) {
        ESP_LOGW(TAG, "ESP-NOW send failed to %02X:%02X:%02X:%02X:%02X:%02X",
                 mac_addr[0], mac_addr[1], mac_addr[2],
                 mac_addr[3], mac_addr[4], mac_addr[5]);
    }
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
static void espnow_recv_cb(const esp_now_recv_info_t *recv_info,
                            const uint8_t *data, int data_len)
{
    const uint8_t *mac_addr = recv_info->src_addr;
#else
static void espnow_recv_cb(const uint8_t *mac_addr,
                            const uint8_t *data, int data_len)
{
#endif
    if (!espnow_state.rx_queue) return;
    if (data_len > ESPNOW_MAX_PAYLOAD) data_len = ESPNOW_MAX_PAYLOAD;

    espnow_rx_item_t item;
    memcpy(item.mac, mac_addr, 6);
    memcpy(item.data, data, data_len);
    item.len = data_len;

    /* ISR-safe send (ESP-NOW recv callback may run from WiFi task) */
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xQueueSendFromISR(espnow_state.rx_queue, &item, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

/* ---------------------------------------------------------------------------
 * Listener background task
 * ---------------------------------------------------------------------------*/

static void espnow_listener_fn(void *param)
{
    (void)param;
    espnow_rx_item_t item;

    while (!espnow_state.listener_stop) {
        if (xQueueReceive(espnow_state.rx_queue, &item, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }

        char mac_str[18];
        format_mac(item.mac, mac_str, sizeof(mac_str));

#ifdef CONFIG_JIM_EXT_MPACK
        if (espnow_state.listener_mpack) {
            /* Deliver as: {proc} {mac} <hex-encoded-mpack> */
            size_t script_len = strlen(espnow_state.listener_proc) + 18 + item.len * 2 + 32;
            char *script = malloc(script_len);
            if (!script) continue;

            int off = snprintf(script, script_len, "%s {%s} ",
                               espnow_state.listener_proc, mac_str);
            for (int i = 0; i < item.len; i++) {
                off += snprintf(script + off, script_len - off, "%02x", item.data[i]);
            }

            if (task_send_to_name(espnow_state.listener_target, script) != 0) {
                ESP_LOGW(TAG, "ESP-NOW listener delivery failed -> task '%s'",
                         espnow_state.listener_target);
            }
            free(script);
        } else
#endif
        {
            /* Raw mode: deliver as {proc} {mac} {data} */
            size_t script_len = strlen(espnow_state.listener_proc) + 18 + item.len * 4 + 32;
            char *script = malloc(script_len);
            if (!script) continue;

            int off = snprintf(script, script_len, "%s {%s} {",
                               espnow_state.listener_proc, mac_str);
            for (int i = 0; i < item.len; i++) {
                uint8_t ch = item.data[i];
                if (ch >= 0x20 && ch < 0x7f &&
                    ch != '{' && ch != '}' && ch != '\\') {
                    script[off++] = (char)ch;
                } else {
                    off += snprintf(script + off, 8, "\\x%02x", ch);
                }
            }
            script[off++] = '}';
            script[off] = '\0';

            if (task_send_to_name(espnow_state.listener_target, script) != 0) {
                ESP_LOGW(TAG, "ESP-NOW listener delivery failed -> task '%s'",
                         espnow_state.listener_target);
            }
            free(script);
        }
    }

    ESP_LOGI(TAG, "ESP-NOW listener stopped");
    espnow_state.listener_task = NULL;
    vTaskDelete(NULL);
}

/* ---------------------------------------------------------------------------
 * Subcommand: espnow init
 * ---------------------------------------------------------------------------*/

static int espnow_cmd_init(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    const char *pmk = NULL;

    /* Parse options */
    for (int i = 0; i < argc; i++) {
        const char *opt = Jim_String(argv[i]);
        if (strcmp(opt, "-pmk") == 0 && i + 1 < argc) {
            pmk = Jim_String(argv[++i]);
        } else {
            Jim_SetResultFormatted(interp, "unknown option \"%s\"", opt);
            return JIM_ERR;
        }
    }

    if (espnow_state.initialized) {
        Jim_SetResultString(interp, "ESP-NOW already initialized", -1);
        return JIM_ERR;
    }

    /* Create receive queue */
    espnow_state.rx_queue = xQueueCreate(ESPNOW_RX_QUEUE_LEN, sizeof(espnow_rx_item_t));
    if (!espnow_state.rx_queue) {
        Jim_SetResultString(interp, "failed to create rx queue", -1);
        return JIM_ERR;
    }

    esp_err_t err = esp_now_init();
    if (err != ESP_OK) {
        vQueueDelete(espnow_state.rx_queue);
        espnow_state.rx_queue = NULL;
        Jim_SetResultFormatted(interp, "esp_now_init failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    err = esp_now_register_send_cb(espnow_send_cb);
    if (err != ESP_OK) {
        esp_now_deinit();
        vQueueDelete(espnow_state.rx_queue);
        espnow_state.rx_queue = NULL;
        Jim_SetResultFormatted(interp, "register send cb failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    err = esp_now_register_recv_cb(espnow_recv_cb);
    if (err != ESP_OK) {
        esp_now_deinit();
        vQueueDelete(espnow_state.rx_queue);
        espnow_state.rx_queue = NULL;
        Jim_SetResultFormatted(interp, "register recv cb failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    /* Set Primary Master Key if provided (must be exactly 16 bytes) */
    if (pmk) {
        int pmk_len = strlen(pmk);
        if (pmk_len != 16) {
            esp_now_deinit();
            vQueueDelete(espnow_state.rx_queue);
            espnow_state.rx_queue = NULL;
            Jim_SetResultString(interp, "PMK must be exactly 16 bytes", -1);
            return JIM_ERR;
        }
        err = esp_now_set_pmk((const uint8_t *)pmk);
        if (err != ESP_OK) {
            esp_now_deinit();
            vQueueDelete(espnow_state.rx_queue);
            espnow_state.rx_queue = NULL;
            Jim_SetResultFormatted(interp, "esp_now_set_pmk failed: %s", esp_err_to_name(err));
            return JIM_ERR;
        }
        ESP_LOGI(TAG, "ESP-NOW PMK set");
    }

    espnow_state.initialized = 1;
    ESP_LOGI(TAG, "ESP-NOW initialized");
    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Subcommand: espnow deinit
 * ---------------------------------------------------------------------------*/

static int espnow_cmd_deinit(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    (void)argc; (void)argv;

    if (!espnow_state.initialized) {
        Jim_SetResultString(interp, "ESP-NOW not initialized", -1);
        return JIM_ERR;
    }

    /* Stop listener if running */
    if (espnow_state.listener_task) {
        espnow_state.listener_stop = 1;
        int wait = 0;
        while (espnow_state.listener_task && wait < 20) {
            vTaskDelay(pdMS_TO_TICKS(50));
            wait++;
        }
        if (espnow_state.listener_task) {
            vTaskDelete(espnow_state.listener_task);
            espnow_state.listener_task = NULL;
        }
        espnow_state.listener_stop = 0;
    }

    esp_now_deinit();

    if (espnow_state.rx_queue) {
        vQueueDelete(espnow_state.rx_queue);
        espnow_state.rx_queue = NULL;
    }

    espnow_state.initialized = 0;
    ESP_LOGI(TAG, "ESP-NOW deinitialized");
    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Subcommand: espnow peer add|remove|list
 * ---------------------------------------------------------------------------*/

static int espnow_cmd_peer(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc < 1) {
        Jim_SetResultString(interp,
            "wrong # args: should be \"espnow peer add|remove|list ...\"", -1);
        return JIM_ERR;
    }

    if (!espnow_state.initialized) {
        Jim_SetResultString(interp, "ESP-NOW not initialized", -1);
        return JIM_ERR;
    }

    const char *subcmd = Jim_String(argv[0]);

    /* espnow peer list */
    if (strcmp(subcmd, "list") == 0) {
        esp_now_peer_info_t peer;
        Jim_Obj *listObj = Jim_NewListObj(interp, NULL, 0);

        if (esp_now_fetch_peer(true, &peer) == ESP_OK) {
            do {
                char mac_str[18];
                format_mac(peer.peer_addr, mac_str, sizeof(mac_str));
                Jim_ListAppendElement(interp, listObj,
                    Jim_NewStringObj(interp, mac_str, -1));
            } while (esp_now_fetch_peer(false, &peer) == ESP_OK);
        }

        Jim_SetResult(interp, listObj);
        return JIM_OK;
    }

    /* espnow peer add <mac> ?-channel ch? ?-encrypt 0|1? */
    if (strcmp(subcmd, "add") == 0) {
        if (argc < 2) {
            Jim_SetResultString(interp,
                "wrong # args: should be \"espnow peer add mac ?-channel ch? ?-encrypt 0|1? ?-key <16-byte-key>?\"", -1);
            return JIM_ERR;
        }

        uint8_t mac[6];
        if (parse_mac(Jim_String(argv[1]), mac) != 0) {
            Jim_SetResultFormatted(interp, "invalid MAC address: %s", Jim_String(argv[1]));
            return JIM_ERR;
        }

        esp_now_peer_info_t peer_info = {0};
        memcpy(peer_info.peer_addr, mac, 6);
        peer_info.channel = 0; /* Use current channel */
        peer_info.encrypt = false;

        for (int i = 2; i < argc; i++) {
            const char *opt = Jim_String(argv[i]);
            if (strcmp(opt, "-channel") == 0 && i + 1 < argc) {
                long ch;
                if (Jim_GetLong(interp, argv[++i], &ch) != JIM_OK) return JIM_ERR;
                peer_info.channel = (uint8_t)ch;
            } else if (strcmp(opt, "-encrypt") == 0 && i + 1 < argc) {
                long enc;
                if (Jim_GetLong(interp, argv[++i], &enc) != JIM_OK) return JIM_ERR;
                peer_info.encrypt = enc ? true : false;
            } else if (strcmp(opt, "-key") == 0 && i + 1 < argc) {
                const char *lmk = Jim_String(argv[++i]);
                if (strlen(lmk) != 16) {
                    Jim_SetResultString(interp, "LMK (-key) must be exactly 16 bytes", -1);
                    return JIM_ERR;
                }
                memcpy(peer_info.lmk, lmk, 16);
                peer_info.encrypt = true;  /* Auto-enable encryption when key is set */
            } else {
                Jim_SetResultFormatted(interp, "unknown option \"%s\"", opt);
                return JIM_ERR;
            }
        }

        esp_err_t err = esp_now_add_peer(&peer_info);
        if (err != ESP_OK) {
            Jim_SetResultFormatted(interp, "esp_now_add_peer failed: %s", esp_err_to_name(err));
            return JIM_ERR;
        }

        ESP_LOGI(TAG, "Peer added: %s", Jim_String(argv[1]));
        return JIM_OK;
    }

    /* espnow peer remove <mac> */
    if (strcmp(subcmd, "remove") == 0) {
        if (argc < 2) {
            Jim_SetResultString(interp,
                "wrong # args: should be \"espnow peer remove mac\"", -1);
            return JIM_ERR;
        }

        uint8_t mac[6];
        if (parse_mac(Jim_String(argv[1]), mac) != 0) {
            Jim_SetResultFormatted(interp, "invalid MAC address: %s", Jim_String(argv[1]));
            return JIM_ERR;
        }

        esp_err_t err = esp_now_del_peer(mac);
        if (err != ESP_OK) {
            Jim_SetResultFormatted(interp, "esp_now_del_peer failed: %s", esp_err_to_name(err));
            return JIM_ERR;
        }

        ESP_LOGI(TAG, "Peer removed: %s", Jim_String(argv[1]));
        return JIM_OK;
    }

    Jim_SetResultFormatted(interp, "unknown peer subcommand \"%s\": should be add, remove, or list",
                           subcmd);
    return JIM_ERR;
}

/* ---------------------------------------------------------------------------
 * Subcommand: espnow send <mac> <data> ?-mpack?
 * ---------------------------------------------------------------------------*/

static int espnow_cmd_send(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc < 2) {
        Jim_SetResultString(interp,
            "wrong # args: should be \"espnow send mac data ?-mpack?\"", -1);
        return JIM_ERR;
    }

    if (!espnow_state.initialized) {
        Jim_SetResultString(interp, "ESP-NOW not initialized", -1);
        return JIM_ERR;
    }

    uint8_t mac[6];
    if (parse_mac(Jim_String(argv[0]), mac) != 0) {
        Jim_SetResultFormatted(interp, "invalid MAC address: %s", Jim_String(argv[0]));
        return JIM_ERR;
    }

    int use_mpack = 0;
    for (int i = 2; i < argc; i++) {
        const char *opt = Jim_String(argv[i]);
        if (strcmp(opt, "-mpack") == 0) {
#ifdef CONFIG_JIM_EXT_MPACK
            use_mpack = 1;
#else
            Jim_SetResultString(interp, "mpack support not compiled in", -1);
            return JIM_ERR;
#endif
        } else {
            Jim_SetResultFormatted(interp, "unknown option \"%s\"", opt);
            return JIM_ERR;
        }
    }

#ifdef CONFIG_JIM_EXT_MPACK
    if (use_mpack) {
        size_t mpack_len = 0;
        uint8_t *mpack_data = jim_dict_to_mpack(interp, argv[1], &mpack_len);
        if (!mpack_data) return JIM_ERR;

        if (mpack_len > ESPNOW_MAX_PAYLOAD) {
            free(mpack_data);
            Jim_SetResultFormatted(interp,
                "mpack data too large: %d bytes (max %d)", (int)mpack_len, ESPNOW_MAX_PAYLOAD);
            return JIM_ERR;
        }

        esp_err_t err = esp_now_send(mac, mpack_data, mpack_len);
        free(mpack_data);

        if (err != ESP_OK) {
            Jim_SetResultFormatted(interp, "esp_now_send failed: %s", esp_err_to_name(err));
            return JIM_ERR;
        }

        Jim_SetResultInt(interp, (jim_wide)mpack_len);
        return JIM_OK;
    }
#endif /* CONFIG_JIM_EXT_MPACK */

    /* Raw mode */
    int data_len;
    const char *data = Jim_GetString(argv[1], &data_len);

    if (data_len > ESPNOW_MAX_PAYLOAD) {
        Jim_SetResultFormatted(interp,
            "data too large: %d bytes (max %d)", data_len, ESPNOW_MAX_PAYLOAD);
        return JIM_ERR;
    }

    esp_err_t err = esp_now_send(mac, (const uint8_t *)data, data_len);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "esp_now_send failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    Jim_SetResultInt(interp, data_len);
    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Subcommand: espnow receive ?timeout_ms? ?-mpack?
 * ---------------------------------------------------------------------------*/

static int espnow_cmd_receive(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (!espnow_state.initialized) {
        Jim_SetResultString(interp, "ESP-NOW not initialized", -1);
        return JIM_ERR;
    }

    long timeout_ms = 5000;
    int use_mpack = 0;

    for (int i = 0; i < argc; i++) {
        const char *opt = Jim_String(argv[i]);
        if (strcmp(opt, "-mpack") == 0) {
#ifdef CONFIG_JIM_EXT_MPACK
            use_mpack = 1;
#else
            Jim_SetResultString(interp, "mpack support not compiled in", -1);
            return JIM_ERR;
#endif
        } else {
            if (Jim_GetLong(interp, argv[i], &timeout_ms) != JIM_OK) return JIM_ERR;
        }
    }

    espnow_rx_item_t item;
    if (xQueueReceive(espnow_state.rx_queue, &item, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        Jim_SetResultString(interp, "", 0);
        return JIM_OK;
    }

    char mac_str[18];
    format_mac(item.mac, mac_str, sizeof(mac_str));

    /* Build result dict: {mac <addr> data <payload>} */
    Jim_Obj *resultObj = Jim_NewDictObj(interp, NULL, 0);
    Jim_DictAddElement(interp, resultObj,
        Jim_NewStringObj(interp, "mac", -1),
        Jim_NewStringObj(interp, mac_str, -1));

#ifdef CONFIG_JIM_EXT_MPACK
    if (use_mpack) {
        /* Decode mpack payload and add as 'data' */
        if (jim_mpack_to_dict(interp, item.data, item.len) != JIM_OK) {
            Jim_SetResultFormatted(interp, "mpack decode failed for received data");
            return JIM_ERR;
        }
        Jim_DictAddElement(interp, resultObj,
            Jim_NewStringObj(interp, "data", -1),
            Jim_GetResult(interp));
        Jim_SetResult(interp, resultObj);
        return JIM_OK;
    }
#endif

    Jim_DictAddElement(interp, resultObj,
        Jim_NewStringObj(interp, "data", -1),
        Jim_NewStringObj(interp, (const char *)item.data, item.len));

    Jim_SetResult(interp, resultObj);
    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Subcommand: espnow listen -callback {proc task} ?-mpack? | -remove
 * ---------------------------------------------------------------------------*/

static int espnow_cmd_listen(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc < 1) {
        Jim_SetResultString(interp,
            "wrong # args: should be \"espnow listen -callback {proc task} ?-mpack?\" "
            "or \"espnow listen -remove\"", -1);
        return JIM_ERR;
    }

    if (!espnow_state.initialized) {
        Jim_SetResultString(interp, "ESP-NOW not initialized", -1);
        return JIM_ERR;
    }

    const char *opt = Jim_String(argv[0]);

    /* espnow listen -remove */
    if (strcmp(opt, "-remove") == 0) {
        if (espnow_state.listener_task) {
            espnow_state.listener_stop = 1;
            int wait = 0;
            while (espnow_state.listener_task && wait < 20) {
                vTaskDelay(pdMS_TO_TICKS(50));
                wait++;
            }
            if (espnow_state.listener_task) {
                ESP_LOGW(TAG, "ESP-NOW listener did not stop cleanly");
                vTaskDelete(espnow_state.listener_task);
                espnow_state.listener_task = NULL;
            }
            espnow_state.listener_stop = 0;
            ESP_LOGI(TAG, "ESP-NOW listener removed");
        }
        return JIM_OK;
    }

    /* espnow listen -callback {proc task} ?-mpack? */
    if (strcmp(opt, "-callback") == 0 && argc >= 2) {
        Jim_Obj *cbObj = argv[1];
        if (Jim_ListLength(interp, cbObj) != 2) {
            Jim_SetResultString(interp, "-callback requires {procname target_task}", -1);
            return JIM_ERR;
        }

        const char *proc = Jim_String(Jim_ListGetIndex(interp, cbObj, 0));
        const char *target = Jim_String(Jim_ListGetIndex(interp, cbObj, 1));

        int use_mpack = 0;
        for (int i = 2; i < argc; i++) {
            const char *o = Jim_String(argv[i]);
            if (strcmp(o, "-mpack") == 0) {
#ifdef CONFIG_JIM_EXT_MPACK
                use_mpack = 1;
#else
                Jim_SetResultString(interp, "mpack support not compiled in", -1);
                return JIM_ERR;
#endif
            } else {
                Jim_SetResultFormatted(interp, "unknown option \"%s\"", o);
                return JIM_ERR;
            }
        }

        /* Stop existing listener if any */
        if (espnow_state.listener_task) {
            espnow_state.listener_stop = 1;
            int wait = 0;
            while (espnow_state.listener_task && wait < 20) {
                vTaskDelay(pdMS_TO_TICKS(50));
                wait++;
            }
            if (espnow_state.listener_task) {
                vTaskDelete(espnow_state.listener_task);
                espnow_state.listener_task = NULL;
            }
            espnow_state.listener_stop = 0;
        }

        strncpy(espnow_state.listener_proc, proc, sizeof(espnow_state.listener_proc) - 1);
        espnow_state.listener_proc[sizeof(espnow_state.listener_proc) - 1] = '\0';
        strncpy(espnow_state.listener_target, target, sizeof(espnow_state.listener_target) - 1);
        espnow_state.listener_target[sizeof(espnow_state.listener_target) - 1] = '\0';
        espnow_state.listener_mpack = use_mpack;

        BaseType_t ret = xTaskCreate(espnow_listener_fn, "espnow_listen", 4096, NULL, 6,
                                     &espnow_state.listener_task);
        if (ret != pdPASS) {
            espnow_state.listener_task = NULL;
            Jim_SetResultString(interp, "failed to create listener task", -1);
            return JIM_ERR;
        }

        ESP_LOGI(TAG, "ESP-NOW listener started: %s -> task '%s' (mpack=%d)",
                 proc, target, use_mpack);
        return JIM_OK;
    }

    Jim_SetResultFormatted(interp, "unknown option \"%s\"", opt);
    return JIM_ERR;
}

/* ---------------------------------------------------------------------------
 * Subcommand: espnow status
 * ---------------------------------------------------------------------------*/

static int espnow_cmd_status(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    (void)argc; (void)argv;

    Jim_Obj *resultObj = Jim_NewDictObj(interp, NULL, 0);

    Jim_DictAddElement(interp, resultObj,
        Jim_NewStringObj(interp, "initialized", -1),
        Jim_NewIntObj(interp, espnow_state.initialized));

    if (espnow_state.initialized) {
        /* Count peers */
        esp_now_peer_num_t peer_num = {0};
        esp_now_get_peer_num(&peer_num);
        Jim_DictAddElement(interp, resultObj,
            Jim_NewStringObj(interp, "peer_count", -1),
            Jim_NewIntObj(interp, peer_num.total_num));
        Jim_DictAddElement(interp, resultObj,
            Jim_NewStringObj(interp, "encrypt_count", -1),
            Jim_NewIntObj(interp, peer_num.encrypt_num));
    }

    Jim_DictAddElement(interp, resultObj,
        Jim_NewStringObj(interp, "listener", -1),
        Jim_NewIntObj(interp, espnow_state.listener_task != NULL ? 1 : 0));

    Jim_SetResult(interp, resultObj);
    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Subcommand table and init
 * ---------------------------------------------------------------------------*/

static const jim_subcmd_type espnow_command_table[] = {
    {   "init",
        "?-pmk key?",
        espnow_cmd_init,
        0,
        -1,
        /* Description: Initialize ESP-NOW */
    },
    {   "deinit",
        "",
        espnow_cmd_deinit,
        0,
        0,
        /* Description: Deinitialize ESP-NOW */
    },
    {   "peer",
        "add|remove|list ...",
        espnow_cmd_peer,
        1,
        -1,
        /* Description: Manage ESP-NOW peers */
    },
    {   "send",
        "mac data ?-mpack?",
        espnow_cmd_send,
        2,
        -1,
        /* Description: Send data via ESP-NOW */
    },
    {   "receive",
        "?timeout_ms? ?-mpack?",
        espnow_cmd_receive,
        0,
        -1,
        /* Description: Receive ESP-NOW data */
    },
    {   "listen",
        "-callback {proc task} ?-mpack? | -remove",
        espnow_cmd_listen,
        1,
        -1,
        /* Description: Async listener for ESP-NOW data */
    },
    {   "status",
        "",
        espnow_cmd_status,
        0,
        0,
        /* Description: Get ESP-NOW status */
    },
    { NULL }
};

int Jim_espnowInit(Jim_Interp *interp)
{
    Jim_PackageProvideCheck(interp, "espnow");
    Jim_RegisterSubCmd(interp, "espnow", espnow_command_table, NULL);
    return JIM_OK;
}
