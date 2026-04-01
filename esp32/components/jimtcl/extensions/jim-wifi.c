/* Jim Tcl WiFi Extension for ESP32
 *
 * Provides Tcl commands for WiFi management:
 *
 *   wifi init
 *   wifi connect <ssid> <password>
 *   wifi disconnect
 *   wifi status
 *   wifi scan
 *   wifi ip
 */

#include <string.h>
#include "jim.h"
#include "jim-subcmd.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "jim-wifi";

/* Event group bits */
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define WIFI_MAX_RETRIES    5

typedef struct {
    esp_netif_t *netif;
    EventGroupHandle_t event_group;
    int initialized;
    int connected;
    int retry_count;
    esp_event_handler_instance_t handler_any_id;
    esp_event_handler_instance_t handler_got_ip;
} wifi_state_t;

static wifi_state_t wifi_state = { 0 };

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_state.connected = 0;
        if (wifi_state.retry_count < WIFI_MAX_RETRIES) {
            esp_wifi_connect();
            wifi_state.retry_count++;
            ESP_LOGI(TAG, "Retrying WiFi connection (attempt %d)", wifi_state.retry_count);
        } else {
            xEventGroupSetBits(wifi_state.event_group, WIFI_FAIL_BIT);
            ESP_LOGW(TAG, "WiFi connection failed after %d retries", WIFI_MAX_RETRIES);
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        wifi_state.retry_count = 0;
        wifi_state.connected = 1;
        xEventGroupSetBits(wifi_state.event_group, WIFI_CONNECTED_BIT);
    }
}

static int wifi_cmd_init(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (wifi_state.initialized) {
        return JIM_OK;
    }

    wifi_state.event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_state.netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &wifi_state.handler_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &wifi_state.handler_got_ip));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_state.initialized = 1;
    ESP_LOGI(TAG, "WiFi initialized");
    return JIM_OK;
}

static int wifi_cmd_connect(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (!wifi_state.initialized) {
        /* Auto-init if not done */
        if (wifi_cmd_init(interp, 0, NULL) != JIM_OK) {
            return JIM_ERR;
        }
    }

    const char *ssid = Jim_String(argv[0]);
    const char *password = Jim_String(argv[1]);

    wifi_config_t wifi_config = { 0 };
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = strlen(password) > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    wifi_state.retry_count = 0;
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to SSID: %s", ssid);

    /* Wait for connection or failure */
    EventBits_t bits = xEventGroupWaitBits(wifi_state.event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE,
        pdMS_TO_TICKS(15000));

    if (bits & WIFI_CONNECTED_BIT) {
        Jim_SetResultString(interp, "connected", -1);
        return JIM_OK;
    }
    else if (bits & WIFI_FAIL_BIT) {
        Jim_SetResultString(interp, "connection failed", -1);
        return JIM_ERR;
    }
    else {
        Jim_SetResultString(interp, "connection timeout", -1);
        return JIM_ERR;
    }
}

static int wifi_cmd_disconnect(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (!wifi_state.initialized) {
        Jim_SetResultString(interp, "wifi not initialized", -1);
        return JIM_ERR;
    }
    esp_wifi_disconnect();
    wifi_state.connected = 0;
    Jim_SetResultString(interp, "disconnected", -1);
    return JIM_OK;
}

static int wifi_cmd_status(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (!wifi_state.initialized) {
        Jim_SetResultString(interp, "uninitialized", -1);
        return JIM_OK;
    }
    if (wifi_state.connected) {
        Jim_SetResultString(interp, "connected", -1);
    } else {
        Jim_SetResultString(interp, "disconnected", -1);
    }
    return JIM_OK;
}

static int wifi_cmd_ip(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (!wifi_state.initialized || !wifi_state.connected) {
        Jim_SetResultString(interp, "not connected", -1);
        return JIM_ERR;
    }

    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(wifi_state.netif, &ip_info);

    char ip_str[16];
    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
    Jim_SetResultString(interp, ip_str, -1);
    return JIM_OK;
}

static int wifi_cmd_scan(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (!wifi_state.initialized) {
        if (wifi_cmd_init(interp, 0, NULL) != JIM_OK) {
            return JIM_ERR;
        }
        /* Need to start WiFi for scanning */
        esp_wifi_start();
    }

    wifi_scan_config_t scan_config = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };

    ESP_ERROR_CHECK(esp_wifi_scan_start(&scan_config, true));

    uint16_t ap_count = 0;
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));

    if (ap_count > 20) {
        ap_count = 20;  /* Limit results to conserve memory */
    }

    wifi_ap_record_t *ap_records = malloc(sizeof(wifi_ap_record_t) * ap_count);
    if (!ap_records) {
        Jim_SetResultString(interp, "out of memory", -1);
        return JIM_ERR;
    }

    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_count, ap_records));

    /* Return as list of dicts */
    Jim_Obj *result = Jim_NewListObj(interp, NULL, 0);
    for (int i = 0; i < ap_count; i++) {
        Jim_Obj *entry = Jim_NewListObj(interp, NULL, 0);
        Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, "ssid", -1));
        Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, (char *)ap_records[i].ssid, -1));
        Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, "rssi", -1));
        Jim_ListAppendElement(interp, entry, Jim_NewIntObj(interp, ap_records[i].rssi));
        Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, "channel", -1));
        Jim_ListAppendElement(interp, entry, Jim_NewIntObj(interp, ap_records[i].primary));
        Jim_ListAppendElement(interp, result, entry);
    }

    free(ap_records);
    Jim_SetResult(interp, result);
    return JIM_OK;
}

static const jim_subcmd_type wifi_command_table[] = {
    {   "init",
        NULL,
        wifi_cmd_init,
        0,
        0,
        /* Description: Initialize WiFi subsystem */
    },
    {   "connect",
        "ssid password",
        wifi_cmd_connect,
        2,
        2,
        /* Description: Connect to a WiFi access point */
    },
    {   "disconnect",
        NULL,
        wifi_cmd_disconnect,
        0,
        0,
        /* Description: Disconnect from WiFi */
    },
    {   "status",
        NULL,
        wifi_cmd_status,
        0,
        0,
        /* Description: Return WiFi connection status */
    },
    {   "ip",
        NULL,
        wifi_cmd_ip,
        0,
        0,
        /* Description: Return current IP address */
    },
    {   "scan",
        NULL,
        wifi_cmd_scan,
        0,
        0,
        /* Description: Scan for available WiFi networks */
    },
    { NULL }
};

int Jim_wifiInit(Jim_Interp *interp)
{
    Jim_PackageProvideCheck(interp, "wifi");
    Jim_RegisterSubCmd(interp, "wifi", wifi_command_table, NULL);
    return JIM_OK;
}
