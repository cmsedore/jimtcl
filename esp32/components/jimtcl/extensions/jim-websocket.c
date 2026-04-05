/* Jim Tcl WebSocket Client Extension for ESP32
 *
 * Provides WebSocket client via esp_websocket_client managed component:
 *
 *   ws connect <uri> ?-header {name value}...?
 *   ws send <text>
 *   ws sendbinary <byte_list>
 *   ws receive ?timeout_ms?
 *   ws close
 *   ws status
 */

#include <string.h>
#include <stdlib.h>
#include "jim.h"
#include "jim-subcmd.h"

#ifdef CONFIG_JIM_EXT_JSON
#include "jim-json.h"
#endif

#include "esp_websocket_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

static const char *TAG = "jim-ws";

#define WS_CONNECTED_BIT  BIT0
#define WS_FAIL_BIT       BIT1
#define WS_RX_QUEUE_LEN   8
#define WS_MAX_MSG_LEN    4096

/* Received message pushed to queue */
typedef struct {
    char *data;
    int len;
    int is_binary;  /* 0 = text, 1 = binary */
} ws_rx_msg_t;

typedef struct {
    esp_websocket_client_handle_t client;
    EventGroupHandle_t event_group;
    QueueHandle_t rx_queue;
    int connected;
} ws_state_t;

static ws_state_t ws = { 0 };

/* ---------------------------------------------------------------------------
 * WebSocket event handler
 * ---------------------------------------------------------------------------*/

static void ws_event_handler(void *handler_args, esp_event_base_t base,
                             int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ws.connected = 1;
            xEventGroupSetBits(ws.event_group, WS_CONNECTED_BIT);
            ESP_LOGI(TAG, "WebSocket connected");
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
            ws.connected = 0;
            ESP_LOGW(TAG, "WebSocket disconnected");
            break;

        case WEBSOCKET_EVENT_ERROR:
            xEventGroupSetBits(ws.event_group, WS_FAIL_BIT);
            ESP_LOGE(TAG, "WebSocket error");
            break;

        case WEBSOCKET_EVENT_DATA: {
            if (!ws.rx_queue) break;
            if (!data->data_ptr || data->data_len <= 0) break;
            if (data->data_len > WS_MAX_MSG_LEN) break;

            /* Only queue complete messages (FIN bit set) */
            ws_rx_msg_t msg;
            msg.data = malloc(data->data_len);
            if (!msg.data) break;
            memcpy(msg.data, data->data_ptr, data->data_len);
            msg.len = data->data_len;
            msg.is_binary = (data->op_code == 2);  /* 1=text, 2=binary */

            if (xQueueSend(ws.rx_queue, &msg, 0) != pdTRUE) {
                /* Queue full — drop oldest */
                ws_rx_msg_t old;
                if (xQueueReceive(ws.rx_queue, &old, 0) == pdTRUE) {
                    free(old.data);
                }
                xQueueSend(ws.rx_queue, &msg, 0);
            }
            break;
        }

        default:
            break;
    }
}

/* ---------------------------------------------------------------------------
 * Tcl commands
 * ---------------------------------------------------------------------------*/

static int ws_cmd_connect(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (ws.client) {
        Jim_SetResultString(interp, "already connected — close first", -1);
        return JIM_ERR;
    }

    const char *uri = Jim_String(argv[0]);

    if (!ws.event_group) {
        ws.event_group = xEventGroupCreate();
    }
    if (!ws.rx_queue) {
        ws.rx_queue = xQueueCreate(WS_RX_QUEUE_LEN, sizeof(ws_rx_msg_t));
    }

    xEventGroupClearBits(ws.event_group, WS_CONNECTED_BIT | WS_FAIL_BIT);

    esp_websocket_client_config_t config = {
        .uri = uri,
    };

    /* Parse optional headers */
    char headers_buf[512] = "";
    int hdr_pos = 0;
    for (int i = 1; i < argc; i++) {
        const char *opt = Jim_String(argv[i]);
        if (strcmp(opt, "-header") == 0 && i + 1 < argc) {
            Jim_Obj *hdr = argv[++i];
            if (Jim_ListLength(interp, hdr) != 2) {
                Jim_SetResultString(interp, "-header requires {name value}", -1);
                return JIM_ERR;
            }
            const char *name = Jim_String(Jim_ListGetIndex(interp, hdr, 0));
            const char *value = Jim_String(Jim_ListGetIndex(interp, hdr, 1));
            int written = snprintf(headers_buf + hdr_pos, sizeof(headers_buf) - hdr_pos,
                                   "%s: %s\r\n", name, value);
            if (written > 0) hdr_pos += written;
        } else {
            Jim_SetResultFormatted(interp, "unknown option \"%s\"", opt);
            return JIM_ERR;
        }
    }
    if (hdr_pos > 0) {
        config.headers = headers_buf;
    }

    ws.client = esp_websocket_client_init(&config);
    if (!ws.client) {
        Jim_SetResultString(interp, "failed to create WebSocket client", -1);
        return JIM_ERR;
    }

    esp_websocket_register_events(ws.client, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
    esp_websocket_client_start(ws.client);

    /* Wait for connection */
    EventBits_t bits = xEventGroupWaitBits(ws.event_group,
        WS_CONNECTED_BIT | WS_FAIL_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));

    if (bits & WS_CONNECTED_BIT) {
        Jim_SetResultString(interp, "connected", -1);
        return JIM_OK;
    }

    esp_websocket_client_stop(ws.client);
    esp_websocket_client_destroy(ws.client);
    ws.client = NULL;

    if (bits & WS_FAIL_BIT) {
        Jim_SetResultString(interp, "connection failed", -1);
    } else {
        Jim_SetResultString(interp, "connection timeout", -1);
    }
    return JIM_ERR;
}

static int ws_cmd_send(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (!ws.client || !ws.connected) {
        Jim_SetResultString(interp, "not connected", -1);
        return JIM_ERR;
    }

    int json_flag = 0;
    if (argc >= 2) {
        const char *opt = Jim_String(argv[1]);
        if (strcmp(opt, "-json") == 0) {
            json_flag = 1;
        }
    }

    int len;
    const char *text = Jim_GetString(argv[0], &len);
    int sent;

#ifdef CONFIG_JIM_EXT_JSON
    char *json_str = NULL;
    if (json_flag) {
        json_str = jim_dict_to_json(interp, argv[0]);
        if (!json_str) return JIM_ERR;
        text = json_str;
        len = strlen(json_str);
    }
#else
    if (json_flag) {
        Jim_SetResultString(interp, "JSON extension not enabled", -1);
        return JIM_ERR;
    }
#endif

    sent = esp_websocket_client_send_text(ws.client, text, len, pdMS_TO_TICKS(5000));

#ifdef CONFIG_JIM_EXT_JSON
    free(json_str);
#endif

    if (sent < 0) {
        Jim_SetResultString(interp, "send failed", -1);
        return JIM_ERR;
    }

    Jim_SetResultInt(interp, sent);
    return JIM_OK;
}

static int ws_cmd_sendbinary(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (!ws.client || !ws.connected) {
        Jim_SetResultString(interp, "not connected", -1);
        return JIM_ERR;
    }

    int data_len = Jim_ListLength(interp, argv[0]);
    if (data_len <= 0) {
        Jim_SetResultString(interp, "empty data", -1);
        return JIM_ERR;
    }

    uint8_t *buf = malloc(data_len);
    if (!buf) {
        Jim_SetResultString(interp, "out of memory", -1);
        return JIM_ERR;
    }

    for (int i = 0; i < data_len; i++) {
        Jim_Obj *elem = Jim_ListGetIndex(interp, argv[0], i);
        long val;
        if (Jim_GetLong(interp, elem, &val) != JIM_OK) {
            free(buf);
            return JIM_ERR;
        }
        buf[i] = (uint8_t)(val & 0xFF);
    }

    int sent = esp_websocket_client_send_bin(ws.client, (const char *)buf, data_len, pdMS_TO_TICKS(5000));
    free(buf);

    if (sent < 0) {
        Jim_SetResultString(interp, "send failed", -1);
        return JIM_ERR;
    }

    Jim_SetResultInt(interp, sent);
    return JIM_OK;
}

static int ws_cmd_receive(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (!ws.client || !ws.connected) {
        Jim_SetResultString(interp, "not connected", -1);
        return JIM_ERR;
    }

    long timeout_ms = 5000;
    int json_flag = 0;

    for (int i = 0; i < argc; i++) {
        const char *opt = Jim_String(argv[i]);
        if (strcmp(opt, "-json") == 0) {
            json_flag = 1;
        } else {
            if (Jim_GetLong(interp, argv[i], &timeout_ms) != JIM_OK) return JIM_ERR;
        }
    }

    ws_rx_msg_t msg;
    if (xQueueReceive(ws.rx_queue, &msg, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        Jim_SetResultString(interp, "timeout", -1);
        return JIM_ERR;
    }

    Jim_Obj *result = Jim_NewListObj(interp, NULL, 0);
    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "type", -1));
    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp,
        msg.is_binary ? "binary" : "text", -1));
    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "data", -1));

    if (msg.is_binary) {
        /* Return as byte list */
        Jim_Obj *bytes = Jim_NewListObj(interp, NULL, 0);
        for (int i = 0; i < msg.len; i++) {
            Jim_ListAppendElement(interp, bytes,
                Jim_NewIntObj(interp, (unsigned char)msg.data[i]));
        }
        Jim_ListAppendElement(interp, result, bytes);
    } else {
#ifdef CONFIG_JIM_EXT_JSON
        if (json_flag && msg.len > 0) {
            if (jim_json_to_dict(interp, msg.data, msg.len) == JIM_OK) {
                Jim_ListAppendElement(interp, result, Jim_GetResult(interp));
            } else {
                /* Parse failed — fall back to raw text */
                Jim_ListAppendElement(interp, result,
                    Jim_NewStringObj(interp, msg.data, msg.len));
            }
        } else
#endif
        {
            Jim_ListAppendElement(interp, result,
                Jim_NewStringObj(interp, msg.data, msg.len));
        }
    }

    free(msg.data);
    Jim_SetResult(interp, result);
    return JIM_OK;
}

static int ws_cmd_close(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (!ws.client) {
        Jim_SetResultString(interp, "not connected", -1);
        return JIM_ERR;
    }

    esp_websocket_client_close(ws.client, pdMS_TO_TICKS(5000));
    esp_websocket_client_stop(ws.client);
    esp_websocket_client_destroy(ws.client);
    ws.client = NULL;
    ws.connected = 0;

    Jim_SetResultString(interp, "closed", -1);
    return JIM_OK;
}

static int ws_cmd_status(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (!ws.client) {
        Jim_SetResultString(interp, "disconnected", -1);
    } else if (ws.connected) {
        Jim_SetResultString(interp, "connected", -1);
    } else {
        Jim_SetResultString(interp, "connecting", -1);
    }
    return JIM_OK;
}

static const jim_subcmd_type ws_command_table[] = {
    {   "connect",
        "uri ?-header {name value}...?",
        ws_cmd_connect,
        1,
        -1,
    },
    {   "send",
        "text ?-json?",
        ws_cmd_send,
        1,
        -1,
    },
    {   "sendbinary",
        "byte_list",
        ws_cmd_sendbinary,
        1,
        1,
    },
    {   "receive",
        "?timeout_ms? ?-json?",
        ws_cmd_receive,
        0,
        -1,
    },
    {   "close",
        NULL,
        ws_cmd_close,
        0,
        0,
    },
    {   "status",
        NULL,
        ws_cmd_status,
        0,
        0,
    },
    { NULL }
};

int Jim_websocketInit(Jim_Interp *interp)
{
    Jim_PackageProvideCheck(interp, "websocket");
    Jim_RegisterSubCmd(interp, "ws", ws_command_table, NULL);
    return JIM_OK;
}
