/* Jim Tcl MQTT Client Extension for ESP32
 *
 * Provides MQTT publish/subscribe via the ESP-IDF mqtt component:
 *
 *   mqtt connect <uri> ?-clientid id? ?-username user? ?-password pass?
 *   mqtt publish <topic> <message> ?-qos 0|1|2? ?-retain?
 *   mqtt subscribe <topic> ?-qos 0|1|2?
 *   mqtt unsubscribe <topic>
 *   mqtt receive ?timeout_ms?
 *   mqtt disconnect
 *   mqtt status
 */

#include <string.h>
#include <stdlib.h>
#include "jim.h"
#include "jim-subcmd.h"

#ifdef CONFIG_JIM_EXT_JSON
#include "jim-json.h"
#endif

#include "mqtt_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

static const char *TAG = "jim-mqtt";

#define MQTT_CONNECTED_BIT  BIT0
#define MQTT_FAIL_BIT       BIT1
#define MQTT_RX_QUEUE_LEN   8
#define MQTT_MAX_MSG_LEN    2048

/* Received message pushed to queue */
typedef struct {
    char *topic;
    char *data;
    int topic_len;
    int data_len;
    int qos;
} mqtt_rx_msg_t;

typedef struct {
    esp_mqtt_client_handle_t client;
    EventGroupHandle_t event_group;
    QueueHandle_t rx_queue;
    int connected;
} mqtt_state_t;

static mqtt_state_t mqtt = { 0 };

/* ---------------------------------------------------------------------------
 * MQTT event handler
 * ---------------------------------------------------------------------------*/

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            mqtt.connected = 1;
            xEventGroupSetBits(mqtt.event_group, MQTT_CONNECTED_BIT);
            ESP_LOGI(TAG, "Connected to broker");
            break;

        case MQTT_EVENT_DISCONNECTED:
            mqtt.connected = 0;
            ESP_LOGW(TAG, "Disconnected from broker");
            break;

        case MQTT_EVENT_ERROR:
            xEventGroupSetBits(mqtt.event_group, MQTT_FAIL_BIT);
            ESP_LOGE(TAG, "MQTT error");
            break;

        case MQTT_EVENT_DATA: {
            if (!mqtt.rx_queue) break;
            if (event->data_len > MQTT_MAX_MSG_LEN) break;

            mqtt_rx_msg_t msg;
            msg.topic = strndup(event->topic, event->topic_len);
            msg.data = strndup(event->data, event->data_len);
            msg.topic_len = event->topic_len;
            msg.data_len = event->data_len;
            msg.qos = event->qos;

            if (!msg.topic || !msg.data) {
                free(msg.topic);
                free(msg.data);
                break;
            }

            if (xQueueSend(mqtt.rx_queue, &msg, 0) != pdTRUE) {
                /* Queue full — drop oldest */
                mqtt_rx_msg_t old;
                if (xQueueReceive(mqtt.rx_queue, &old, 0) == pdTRUE) {
                    free(old.topic);
                    free(old.data);
                }
                xQueueSend(mqtt.rx_queue, &msg, 0);
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

static int mqtt_cmd_connect(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (mqtt.client) {
        Jim_SetResultString(interp, "already connected — disconnect first", -1);
        return JIM_ERR;
    }

    const char *uri = Jim_String(argv[0]);
    const char *client_id = NULL;
    const char *username = NULL;
    const char *password = NULL;

    for (int i = 1; i < argc; i++) {
        const char *opt = Jim_String(argv[i]);
        if (strcmp(opt, "-clientid") == 0 && i + 1 < argc) {
            client_id = Jim_String(argv[++i]);
        } else if (strcmp(opt, "-username") == 0 && i + 1 < argc) {
            username = Jim_String(argv[++i]);
        } else if (strcmp(opt, "-password") == 0 && i + 1 < argc) {
            password = Jim_String(argv[++i]);
        } else {
            Jim_SetResultFormatted(interp, "unknown option \"%s\"", opt);
            return JIM_ERR;
        }
    }

    if (!mqtt.event_group) {
        mqtt.event_group = xEventGroupCreate();
    }
    if (!mqtt.rx_queue) {
        mqtt.rx_queue = xQueueCreate(MQTT_RX_QUEUE_LEN, sizeof(mqtt_rx_msg_t));
    }

    xEventGroupClearBits(mqtt.event_group, MQTT_CONNECTED_BIT | MQTT_FAIL_BIT);

    esp_mqtt_client_config_t config = { 0 };
    config.broker.address.uri = uri;
    if (client_id) config.credentials.client_id = client_id;
    if (username) config.credentials.username = username;
    if (password) config.credentials.authentication.password = password;

    mqtt.client = esp_mqtt_client_init(&config);
    if (!mqtt.client) {
        Jim_SetResultString(interp, "failed to create MQTT client", -1);
        return JIM_ERR;
    }

    esp_mqtt_client_register_event(mqtt.client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt.client);

    /* Wait for connection */
    EventBits_t bits = xEventGroupWaitBits(mqtt.event_group,
        MQTT_CONNECTED_BIT | MQTT_FAIL_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));

    if (bits & MQTT_CONNECTED_BIT) {
        Jim_SetResultString(interp, "connected", -1);
        return JIM_OK;
    }

    /* Failed */
    esp_mqtt_client_stop(mqtt.client);
    esp_mqtt_client_destroy(mqtt.client);
    mqtt.client = NULL;

    if (bits & MQTT_FAIL_BIT) {
        Jim_SetResultString(interp, "connection failed", -1);
    } else {
        Jim_SetResultString(interp, "connection timeout", -1);
    }
    return JIM_ERR;
}

static int mqtt_cmd_disconnect(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (!mqtt.client) {
        Jim_SetResultString(interp, "not connected", -1);
        return JIM_ERR;
    }

    esp_mqtt_client_stop(mqtt.client);
    esp_mqtt_client_destroy(mqtt.client);
    mqtt.client = NULL;
    mqtt.connected = 0;

    Jim_SetResultString(interp, "disconnected", -1);
    return JIM_OK;
}

static int mqtt_cmd_publish(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (!mqtt.client || !mqtt.connected) {
        Jim_SetResultString(interp, "not connected", -1);
        return JIM_ERR;
    }

    const char *topic = Jim_String(argv[0]);
    int data_len;
    const char *data = Jim_GetString(argv[1], &data_len);
    long qos = 0;
    int retain = 0;
    int json_flag = 0;

    for (int i = 2; i < argc; i++) {
        const char *opt = Jim_String(argv[i]);
        if (strcmp(opt, "-qos") == 0 && i + 1 < argc) {
            if (Jim_GetLong(interp, argv[++i], &qos) != JIM_OK) return JIM_ERR;
            if (qos < 0 || qos > 2) {
                Jim_SetResultString(interp, "QoS must be 0, 1, or 2", -1);
                return JIM_ERR;
            }
        } else if (strcmp(opt, "-retain") == 0) {
            retain = 1;
        } else if (strcmp(opt, "-json") == 0) {
            json_flag = 1;
        } else {
            Jim_SetResultFormatted(interp, "unknown option \"%s\"", opt);
            return JIM_ERR;
        }
    }

#ifdef CONFIG_JIM_EXT_JSON
    char *json_str = NULL;
    if (json_flag) {
        json_str = jim_dict_to_json(interp, argv[1]);
        if (!json_str) return JIM_ERR;
        data = json_str;
        data_len = strlen(json_str);
    }
#else
    if (json_flag) {
        Jim_SetResultString(interp, "JSON extension not enabled", -1);
        return JIM_ERR;
    }
#endif

    int msg_id = esp_mqtt_client_publish(mqtt.client, topic, data, data_len, (int)qos, retain);

#ifdef CONFIG_JIM_EXT_JSON
    free(json_str);
#endif

    if (msg_id < 0) {
        Jim_SetResultString(interp, "publish failed", -1);
        return JIM_ERR;
    }

    Jim_SetResultInt(interp, msg_id);
    return JIM_OK;
}

static int mqtt_cmd_subscribe(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (!mqtt.client || !mqtt.connected) {
        Jim_SetResultString(interp, "not connected", -1);
        return JIM_ERR;
    }

    const char *topic = Jim_String(argv[0]);
    long qos = 0;

    if (argc >= 3) {
        const char *opt = Jim_String(argv[1]);
        if (strcmp(opt, "-qos") == 0) {
            if (Jim_GetLong(interp, argv[2], &qos) != JIM_OK) return JIM_ERR;
        }
    }

    int msg_id = esp_mqtt_client_subscribe_single(mqtt.client, topic, (int)qos);
    if (msg_id < 0) {
        Jim_SetResultString(interp, "subscribe failed", -1);
        return JIM_ERR;
    }

    Jim_SetResultInt(interp, msg_id);
    return JIM_OK;
}

static int mqtt_cmd_unsubscribe(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (!mqtt.client || !mqtt.connected) {
        Jim_SetResultString(interp, "not connected", -1);
        return JIM_ERR;
    }

    const char *topic = Jim_String(argv[0]);
    int msg_id = esp_mqtt_client_unsubscribe(mqtt.client, topic);
    if (msg_id < 0) {
        Jim_SetResultString(interp, "unsubscribe failed", -1);
        return JIM_ERR;
    }

    Jim_SetResultInt(interp, msg_id);
    return JIM_OK;
}

static int mqtt_cmd_receive(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (!mqtt.client || !mqtt.connected) {
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
            /* Try as timeout */
            if (Jim_GetLong(interp, argv[i], &timeout_ms) != JIM_OK) return JIM_ERR;
        }
    }

    mqtt_rx_msg_t msg;
    if (xQueueReceive(mqtt.rx_queue, &msg, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        Jim_SetResultString(interp, "timeout", -1);
        return JIM_ERR;
    }

    Jim_Obj *result = Jim_NewListObj(interp, NULL, 0);
    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "topic", -1));
    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, msg.topic, msg.topic_len));
    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "data", -1));

#ifdef CONFIG_JIM_EXT_JSON
    if (json_flag && msg.data_len > 0) {
        if (jim_json_to_dict(interp, msg.data, msg.data_len) == JIM_OK) {
            Jim_ListAppendElement(interp, result, Jim_GetResult(interp));
        } else {
            /* Parse failed — fall back to raw string */
            Jim_ListAppendElement(interp, result,
                Jim_NewStringObj(interp, msg.data, msg.data_len));
        }
    } else
#endif
    {
        Jim_ListAppendElement(interp, result,
            Jim_NewStringObj(interp, msg.data, msg.data_len));
    }

    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "qos", -1));
    Jim_ListAppendElement(interp, result, Jim_NewIntObj(interp, msg.qos));

    free(msg.topic);
    free(msg.data);
    Jim_SetResult(interp, result);
    return JIM_OK;
}

static int mqtt_cmd_status(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (!mqtt.client) {
        Jim_SetResultString(interp, "disconnected", -1);
    } else if (mqtt.connected) {
        Jim_SetResultString(interp, "connected", -1);
    } else {
        Jim_SetResultString(interp, "connecting", -1);
    }
    return JIM_OK;
}

static const jim_subcmd_type mqtt_command_table[] = {
    {   "connect",
        "uri ?-clientid id? ?-username user? ?-password pass?",
        mqtt_cmd_connect,
        1,
        -1,
    },
    {   "disconnect",
        NULL,
        mqtt_cmd_disconnect,
        0,
        0,
    },
    {   "publish",
        "topic message ?-qos 0|1|2? ?-retain? ?-json?",
        mqtt_cmd_publish,
        2,
        -1,
    },
    {   "subscribe",
        "topic ?-qos 0|1|2?",
        mqtt_cmd_subscribe,
        1,
        3,
    },
    {   "unsubscribe",
        "topic",
        mqtt_cmd_unsubscribe,
        1,
        1,
    },
    {   "receive",
        "?timeout_ms? ?-json?",
        mqtt_cmd_receive,
        0,
        -1,
    },
    {   "status",
        NULL,
        mqtt_cmd_status,
        0,
        0,
    },
    { NULL }
};

int Jim_mqttInit(Jim_Interp *interp)
{
    Jim_PackageProvideCheck(interp, "mqtt");
    Jim_RegisterSubCmd(interp, "mqtt", mqtt_command_table, NULL);
    return JIM_OK;
}
