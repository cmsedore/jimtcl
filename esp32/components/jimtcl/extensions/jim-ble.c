/* Jim Tcl BLE (NimBLE) Extension for ESP32
 *
 * Provides Tcl commands for Bluetooth Low Energy communication using NimBLE:
 *
 *   ble init ?-name <device_name>?
 *   ble deinit
 *   ble address
 *   ble service add {uuid <UUID> chars {{uuid <UUID> props {read write notify ...} ?value <val>?} ...}}
 *   ble service list
 *   ble advertise start ?-name <name>? ?-uuid <16bit_uuid>?
 *   ble advertise stop
 *   ble char set <handle> <value>
 *   ble char get <handle>
 *   ble notify <handle> ?-conn <conn_handle>?
 *   ble scan start ?-duration <ms>? ?-active 0|1? ?-callback {proc task}?
 *   ble scan stop
 *   ble scan results
 *   ble connect <addr> ?-addr_type public|random? ?-timeout <ms>?
 *   ble disconnect <conn_handle>
 *   ble discover <conn_handle>
 *   ble read <conn_handle> <attr_handle>
 *   ble write <conn_handle> <attr_handle> <data>
 *   ble subscribe <conn_handle> <cccd_handle> ?-notify? ?-indicate?
 *   ble status
 *   ble connections
 *   ble on <event> ?-callback {proc task}?
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "jim.h"
#include "jim-subcmd.h"
#include "jim-esp32-task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#ifdef CONFIG_JIM_EXT_MPACK
#include "jim-mpack.h"
#endif

static const char *TAG = "jim-ble";

/* ---------------------------------------------------------------------------
 * Constants and limits
 * ---------------------------------------------------------------------------*/

#define BLE_MAX_SERVICES    4
#define BLE_MAX_CHARS       8   /* per service */
#define BLE_MAX_CONNECTIONS 3
#define BLE_MAX_SCAN        20
#define BLE_EVENT_QUEUE_LEN 16

#define BLE_SYNC_BIT BIT0

/* ---------------------------------------------------------------------------
 * Type definitions
 * ---------------------------------------------------------------------------*/

typedef struct {
    uint16_t handle;        /* GATT attribute handle (filled by NimBLE on register) */
    ble_uuid_any_t uuid;    /* parsed UUID */
    uint8_t props;          /* BLE_GATT_CHR_F_* flags */
    uint8_t value[256];     /* current value */
    int value_len;
    /* per-char write callback */
    char notify_proc[64];
    char notify_target[16];
    int mpack_mode;
} ble_char_t;

typedef struct {
    int active;
    ble_uuid_any_t uuid;
    ble_char_t chars[BLE_MAX_CHARS];
    int char_count;
} ble_service_t;

typedef struct {
    uint16_t conn_handle;
    uint8_t peer_addr[6];
    int addr_type;
    int connected;
} ble_conn_t;

typedef struct {
    uint8_t addr[6];
    int8_t rssi;
    char name[32];
    int addr_type;
} ble_scan_result_t;

/* Event types for the dispatcher */
typedef enum {
    BLE_EVT_CONNECT,
    BLE_EVT_DISCONNECT,
    BLE_EVT_SCAN_RESULT,
    BLE_EVT_SCAN_DONE,
    BLE_EVT_CHAR_WRITE,     /* client wrote to our characteristic */
    BLE_EVT_NOTIFY,          /* incoming notification (central mode) */
    BLE_EVT_MTU,
    BLE_EVT_SUBSCRIBE,
    BLE_EVT_PASSKEY,         /* passkey action (display or entry) */
    BLE_EVT_ENC_CHANGE,      /* encryption state changed */
} ble_evt_type_t;

typedef struct {
    ble_evt_type_t type;
    uint16_t conn_handle;
    uint8_t addr[6];
    int8_t rssi;
    char name[32];
    int addr_type;
    uint16_t char_handle;
    uint8_t data[256];
    int data_len;
    int status;              /* 0=ok, nonzero=error */
    uint16_t mtu;
    uint8_t passkey_action;  /* BLE_SM_IOACT_DISP, BLE_SM_IOACT_INPUT, etc */
    uint32_t passkey;        /* passkey value (for display or numcmp) */
    int encrypted;           /* encryption state after ENC_CHANGE */
} ble_event_t;

/* Synchronous operation completion */
typedef struct {
    SemaphoreHandle_t sem;
    int status;
    uint8_t data[256];
    int data_len;
} ble_sync_op_t;

typedef struct {
    int initialized;
    uint8_t own_addr_type;
    int advertising;
    QueueHandle_t event_queue;
    TaskHandle_t dispatch_task;
    EventGroupHandle_t sync_group;  /* for waiting on ble_hs_cfg.sync_cb */

    /* Event callbacks */
    char on_connect_proc[64];    char on_connect_target[16];
    char on_disconnect_proc[64]; char on_disconnect_target[16];
    char on_scan_proc[64];       char on_scan_target[16];
    char on_write_proc[64];      char on_write_target[16];
    char on_passkey_proc[64];    char on_passkey_target[16];

    /* Security configuration */
    int sm_bonding;             /* 1 = save bonds to NVS */
    int sm_mitm;                /* 1 = require MITM protection */
    int sm_sc;                  /* 1 = Secure Connections (BLE 4.2+) */
    int sm_io_cap;              /* BLE_SM_IO_CAP_* value */
    uint32_t sm_passkey;        /* fixed passkey for display mode */

    /* GATT server */
    ble_service_t services[BLE_MAX_SERVICES];
    int service_count;

    /* For NimBLE: we need to build ble_gatt_svc_def array.
     * These are heap-allocated and must persist for the lifetime of the
     * NimBLE stack. */
    struct ble_gatt_svc_def *gatt_svcs;
    struct ble_gatt_chr_def **gatt_chr_arrays;  /* one array per service */
    ble_uuid_any_t *gatt_svc_uuids;
    ble_uuid_any_t **gatt_chr_uuids;            /* one array per service */

    /* Connections */
    ble_conn_t connections[BLE_MAX_CONNECTIONS];

    /* Scan results */
    ble_scan_result_t scan_results[BLE_MAX_SCAN];
    int scan_count;
    int scanning;

    /* Sync op for blocking read/write from central side */
    ble_sync_op_t sync_op;

    /* Device name */
    char device_name[32];
} ble_state_t;

static ble_state_t ble = {0};

/* ---------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------------*/

static void format_addr(const uint8_t addr[6], char *buf)
{
    snprintf(buf, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

static int parse_addr(const char *str, uint8_t addr[6])
{
    unsigned int m[6];
    if (sscanf(str, "%x:%x:%x:%x:%x:%x",
               &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6) {
        return -1;
    }
    for (int i = 0; i < 6; i++) {
        if (m[i] > 0xFF) return -1;
        addr[i] = (uint8_t)m[i];
    }
    return 0;
}

/* Parse a UUID string.  Supports:
 *   4 hex chars   -> 16-bit UUID (e.g. "180F")
 *   8-4-4-4-12   -> 128-bit UUID (e.g. "12345678-1234-1234-1234-123456789ABC")
 */
static int parse_uuid(const char *str, ble_uuid_any_t *uuid)
{
    int len = strlen(str);

    if (len == 4) {
        uuid->u16.u.type = BLE_UUID_TYPE_16;
        uuid->u16.value = (uint16_t)strtoul(str, NULL, 16);
        return 0;
    }

    if (len == 36 && str[8] == '-' && str[13] == '-' &&
        str[18] == '-' && str[23] == '-') {
        /* 128-bit UUID: XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX
         * NimBLE stores 128-bit UUIDs in little-endian byte order */
        uint8_t buf[16];
        unsigned int b;
        const char *p = str;
        int idx = 15;  /* fill from MSB end, store in LE */

        /* Parse groups: 4-2-2-2-6 bytes = 16 bytes total */
        /* Group 1: 8 hex chars = 4 bytes */
        for (int i = 0; i < 4; i++) {
            if (sscanf(p, "%2x", &b) != 1) return -1;
            buf[idx--] = (uint8_t)b;
            p += 2;
        }
        p++; /* skip '-' */

        /* Group 2: 4 hex chars = 2 bytes */
        for (int i = 0; i < 2; i++) {
            if (sscanf(p, "%2x", &b) != 1) return -1;
            buf[idx--] = (uint8_t)b;
            p += 2;
        }
        p++;

        /* Group 3: 4 hex chars = 2 bytes */
        for (int i = 0; i < 2; i++) {
            if (sscanf(p, "%2x", &b) != 1) return -1;
            buf[idx--] = (uint8_t)b;
            p += 2;
        }
        p++;

        /* Group 4: 4 hex chars = 2 bytes */
        for (int i = 0; i < 2; i++) {
            if (sscanf(p, "%2x", &b) != 1) return -1;
            buf[idx--] = (uint8_t)b;
            p += 2;
        }
        p++;

        /* Group 5: 12 hex chars = 6 bytes */
        for (int i = 0; i < 6; i++) {
            if (sscanf(p, "%2x", &b) != 1) return -1;
            buf[idx--] = (uint8_t)b;
            p += 2;
        }

        uuid->u128.u.type = BLE_UUID_TYPE_128;
        memcpy(uuid->u128.value, buf, 16);
        return 0;
    }

    return -1;
}

static void uuid_to_str(const ble_uuid_any_t *uuid, char *buf, size_t buflen)
{
    if (uuid->u.type == BLE_UUID_TYPE_16) {
        snprintf(buf, buflen, "%04X", uuid->u16.value);
    } else {
        ble_uuid_to_str(&uuid->u, buf);
    }
}

/* Parse BLE characteristic property strings to NimBLE flags */
static uint16_t parse_chr_flags(Jim_Interp *interp, Jim_Obj *propsObj)
{
    uint16_t flags = 0;
    int len = Jim_ListLength(interp, propsObj);

    for (int i = 0; i < len; i++) {
        const char *prop = Jim_String(Jim_ListGetIndex(interp, propsObj, i));
        if (strcmp(prop, "read") == 0) {
            flags |= BLE_GATT_CHR_F_READ;
        } else if (strcmp(prop, "write") == 0) {
            flags |= BLE_GATT_CHR_F_WRITE;
        } else if (strcmp(prop, "write_no_rsp") == 0) {
            flags |= BLE_GATT_CHR_F_WRITE_NO_RSP;
        } else if (strcmp(prop, "notify") == 0) {
            flags |= BLE_GATT_CHR_F_NOTIFY;
        } else if (strcmp(prop, "indicate") == 0) {
            flags |= BLE_GATT_CHR_F_INDICATE;
        }
    }
    return flags;
}

/* Extract device name from advertisement data */
static void extract_adv_name(const uint8_t *data, uint8_t len, char *name, size_t name_len)
{
    name[0] = '\0';
    int pos = 0;
    while (pos < len) {
        uint8_t field_len = data[pos];
        if (field_len == 0 || pos + field_len >= len) break;
        uint8_t field_type = data[pos + 1];
        if (field_type == BLE_HS_ADV_TYPE_COMP_NAME ||
            field_type == BLE_HS_ADV_TYPE_INCOMP_NAME) {
            int copy_len = field_len - 1;
            if (copy_len >= (int)name_len) copy_len = (int)name_len - 1;
            memcpy(name, &data[pos + 2], copy_len);
            name[copy_len] = '\0';
            return;
        }
        pos += field_len + 1;
    }
}

/* Find or allocate a connection slot */
static int find_conn_slot(uint16_t conn_handle)
{
    for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
        if (ble.connections[i].connected && ble.connections[i].conn_handle == conn_handle) {
            return i;
        }
    }
    return -1;
}

static int alloc_conn_slot(void)
{
    for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
        if (!ble.connections[i].connected) {
            return i;
        }
    }
    return -1;
}

/* Find a characteristic by its attribute handle across all services */
static ble_char_t *find_char_by_handle(uint16_t handle)
{
    for (int s = 0; s < ble.service_count; s++) {
        if (!ble.services[s].active) continue;
        for (int c = 0; c < ble.services[s].char_count; c++) {
            if (ble.services[s].chars[c].handle == handle) {
                return &ble.services[s].chars[c];
            }
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * NimBLE callbacks (run in NimBLE context -- must not do Tcl work)
 * ---------------------------------------------------------------------------*/

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host reset; reason=%d", reason);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr failed; rc=%d", rc);
    }
    rc = ble_hs_id_infer_auto(0, &ble.own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed; rc=%d", rc);
    }
    xEventGroupSetBits(ble.sync_group, BLE_SYNC_BIT);
}

static void ble_host_task(void *param)
{
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();  /* blocks until nimble_port_stop() */
    nimble_port_freertos_deinit();
}

/* GATT server access callback */
static int gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    /* arg encodes service_idx * BLE_MAX_CHARS + char_idx */
    int idx = (int)(intptr_t)arg;
    int svc_idx = idx / BLE_MAX_CHARS;
    int chr_idx = idx % BLE_MAX_CHARS;

    if (svc_idx >= BLE_MAX_SERVICES || chr_idx >= BLE_MAX_CHARS) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    ble_char_t *chr = &ble.services[svc_idx].chars[chr_idx];

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR: {
        int rc = os_mbuf_append(ctxt->om, chr->value, chr->value_len);
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    case BLE_GATT_ACCESS_OP_WRITE_CHR: {
        uint16_t om_len = OS_MBUF_PKTLEN(ctxt->om);
        if (om_len > sizeof(chr->value)) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        int rc = ble_hs_mbuf_to_flat(ctxt->om, chr->value, sizeof(chr->value), NULL);
        if (rc != 0) return BLE_ATT_ERR_UNLIKELY;
        chr->value_len = om_len;

        /* Post write event to dispatcher */
        ble_event_t evt = {0};
        evt.type = BLE_EVT_CHAR_WRITE;
        evt.conn_handle = conn_handle;
        evt.char_handle = attr_handle;
        if (om_len <= sizeof(evt.data)) {
            memcpy(evt.data, chr->value, om_len);
            evt.data_len = om_len;
        }
        if (ble.event_queue) {
            xQueueSend(ble.event_queue, &evt, 0);
        }
        return 0;
    }

    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

/* GATT registration callback -- captures val_handle for characteristics */
static void gatt_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_REGISTER_OP_CHR) {
        /* Walk services to find the matching UUID and set the handle */
        for (int s = 0; s < ble.service_count; s++) {
            if (!ble.services[s].active) continue;
            for (int c = 0; c < ble.services[s].char_count; c++) {
                if (ble_uuid_cmp(&ble.services[s].chars[c].uuid.u,
                                  ctxt->chr.chr_def->uuid) == 0) {
                    ble.services[s].chars[c].handle = ctxt->chr.val_handle;
                    ESP_LOGI(TAG, "Char registered: val_handle=%d", ctxt->chr.val_handle);
                    return;
                }
            }
        }
    }
}

/* GAP event callback -- runs in NimBLE context */
static int ble_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    ble_event_t evt = {0};

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        evt.type = BLE_EVT_CONNECT;
        evt.conn_handle = event->connect.conn_handle;
        evt.status = event->connect.status;
        if (event->connect.status == 0) {
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->connect.conn_handle, &desc) == 0) {
                memcpy(evt.addr, desc.peer_ota_addr.val, 6);
                evt.addr_type = desc.peer_ota_addr.type;
            }
            /* Store in connections array */
            int slot = alloc_conn_slot();
            if (slot >= 0) {
                ble.connections[slot].conn_handle = event->connect.conn_handle;
                memcpy(ble.connections[slot].peer_addr, evt.addr, 6);
                ble.connections[slot].addr_type = evt.addr_type;
                ble.connections[slot].connected = 1;
            }
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        evt.type = BLE_EVT_DISCONNECT;
        evt.conn_handle = event->disconnect.conn.conn_handle;
        evt.status = event->disconnect.reason;
        memcpy(evt.addr, event->disconnect.conn.peer_ota_addr.val, 6);
        /* Remove from connections */
        {
            int slot = find_conn_slot(event->disconnect.conn.conn_handle);
            if (slot >= 0) {
                ble.connections[slot].connected = 0;
            }
        }
        /* If peripheral mode, restart advertising */
        if (ble.advertising) {
            ble.advertising = 0;  /* will be restarted by user or on_disconnect handler */
        }
        break;

    case BLE_GAP_EVENT_DISC:
        evt.type = BLE_EVT_SCAN_RESULT;
        memcpy(evt.addr, event->disc.addr.val, 6);
        evt.addr_type = event->disc.addr.type;
        evt.rssi = event->disc.rssi;
        /* Extract name from advertisement data */
        extract_adv_name(event->disc.data, event->disc.length_data,
                         evt.name, sizeof(evt.name));
        /* Store in scan results array */
        if (ble.scan_count < BLE_MAX_SCAN) {
            /* Check for duplicate addresses */
            int dup = 0;
            for (int i = 0; i < ble.scan_count; i++) {
                if (memcmp(ble.scan_results[i].addr, evt.addr, 6) == 0) {
                    /* Update RSSI and name if improved */
                    ble.scan_results[i].rssi = evt.rssi;
                    if (evt.name[0] && !ble.scan_results[i].name[0]) {
                        strncpy(ble.scan_results[i].name, evt.name,
                                sizeof(ble.scan_results[i].name) - 1);
                    }
                    dup = 1;
                    break;
                }
            }
            if (!dup) {
                ble_scan_result_t *r = &ble.scan_results[ble.scan_count];
                memcpy(r->addr, evt.addr, 6);
                r->rssi = evt.rssi;
                r->addr_type = evt.addr_type;
                strncpy(r->name, evt.name, sizeof(r->name) - 1);
                ble.scan_count++;
            }
        }
        break;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        evt.type = BLE_EVT_SCAN_DONE;
        ble.scanning = 0;
        break;

    case BLE_GAP_EVENT_NOTIFY_RX:
        evt.type = BLE_EVT_NOTIFY;
        evt.conn_handle = event->notify_rx.conn_handle;
        evt.char_handle = event->notify_rx.attr_handle;
        {
            uint16_t om_len = OS_MBUF_PKTLEN(event->notify_rx.om);
            if (om_len > sizeof(evt.data)) om_len = sizeof(evt.data);
            ble_hs_mbuf_to_flat(event->notify_rx.om, evt.data, sizeof(evt.data), NULL);
            evt.data_len = om_len;
        }
        break;

    case BLE_GAP_EVENT_MTU:
        evt.type = BLE_EVT_MTU;
        evt.conn_handle = event->mtu.conn_handle;
        evt.mtu = event->mtu.value;
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        evt.type = BLE_EVT_SUBSCRIBE;
        evt.conn_handle = event->subscribe.conn_handle;
        evt.char_handle = event->subscribe.attr_handle;
        ESP_LOGI(TAG, "Subscribe: conn=%d attr=%d notify=%d indicate=%d",
                 event->subscribe.conn_handle, event->subscribe.attr_handle,
                 event->subscribe.cur_notify, event->subscribe.cur_indicate);
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ble.advertising = 0;
        ESP_LOGI(TAG, "Advertising complete; reason=%d", event->adv_complete.reason);
        break;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc desc;
        ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        evt.type = BLE_EVT_PASSKEY;
        evt.conn_handle = event->passkey.conn_handle;
        evt.passkey_action = event->passkey.params.action;

        if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
            /* We need to display a passkey */
            uint32_t pk = ble.sm_passkey;
            if (pk == 0) {
                /* Generate random passkey */
                pk = (esp_random() % 999999) + 1;
            }
            evt.passkey = pk;
            struct ble_sm_io pkey = {0};
            pkey.action = BLE_SM_IOACT_DISP;
            pkey.passkey = pk;
            ble_sm_inject_io(event->passkey.conn_handle, &pkey);
            ESP_LOGI(TAG, "Passkey display: %06lu", (unsigned long)pk);
        }
        else if (event->passkey.params.action == BLE_SM_IOACT_INPUT) {
            /* We need to input a passkey — deliver to Tcl callback */
            evt.passkey = 0;
            ESP_LOGI(TAG, "Passkey input required on conn %d", event->passkey.conn_handle);
        }
        else if (event->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
            /* Numeric comparison — display and confirm */
            evt.passkey = event->passkey.params.numcmp;
            ESP_LOGI(TAG, "Numeric comparison: %06lu", (unsigned long)event->passkey.params.numcmp);
            /* Auto-confirm for now (Tcl callback can override) */
            struct ble_sm_io pkey = {0};
            pkey.action = BLE_SM_IOACT_NUMCMP;
            pkey.numcmp_accept = 1;
            ble_sm_inject_io(event->passkey.conn_handle, &pkey);
        }
        else if (event->passkey.params.action == BLE_SM_IOACT_NONE) {
            /* Just Works — nothing to do */
            evt.passkey = 0;
        }
        break;
    }

    case BLE_GAP_EVENT_ENC_CHANGE: {
        evt.type = BLE_EVT_ENC_CHANGE;
        evt.conn_handle = event->enc_change.conn_handle;
        evt.status = event->enc_change.status;
        evt.encrypted = (event->enc_change.status == 0) ? 1 : 0;
        ESP_LOGI(TAG, "Encryption change: conn=%d status=%d",
                 event->enc_change.conn_handle, event->enc_change.status);
        break;
    }

    default:
        return 0;
    }

    if (ble.event_queue) {
        xQueueSend(ble.event_queue, &evt, 0);
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Dispatcher task -- reads events from queue and delivers to Tcl tasks
 * ---------------------------------------------------------------------------*/

static void ble_dispatch_task_fn(void *param)
{
    ble_event_t evt;
    char script[320];

    while (1) {
        if (xQueueReceive(ble.event_queue, &evt, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        char addr_str[18];
        format_addr(evt.addr, addr_str);

        switch (evt.type) {
        case BLE_EVT_CONNECT:
            if (ble.on_connect_proc[0]) {
                snprintf(script, sizeof(script), "%s %d %s %d",
                         ble.on_connect_proc, evt.conn_handle, addr_str, evt.status);
                task_send_to_name(ble.on_connect_target, script);
            }
            /* Also signal sync_op semaphore for blocking connect */
            if (ble.sync_op.sem) {
                ble.sync_op.status = evt.status;
                if (evt.status == 0) {
                    /* Store conn_handle in data as 2 bytes */
                    ble.sync_op.data[0] = (uint8_t)(evt.conn_handle & 0xFF);
                    ble.sync_op.data[1] = (uint8_t)(evt.conn_handle >> 8);
                    ble.sync_op.data_len = 2;
                }
                xSemaphoreGive(ble.sync_op.sem);
            }
            break;

        case BLE_EVT_DISCONNECT:
            if (ble.on_disconnect_proc[0]) {
                snprintf(script, sizeof(script), "%s %d %s %d",
                         ble.on_disconnect_proc, evt.conn_handle, addr_str, evt.status);
                task_send_to_name(ble.on_disconnect_target, script);
            }
            break;

        case BLE_EVT_SCAN_RESULT:
            if (ble.on_scan_proc[0]) {
                snprintf(script, sizeof(script), "%s {%s} %d {%s} %d",
                         ble.on_scan_proc, addr_str, evt.rssi,
                         evt.name, evt.addr_type);
                task_send_to_name(ble.on_scan_target, script);
            }
            break;

        case BLE_EVT_SCAN_DONE:
            if (ble.on_scan_proc[0]) {
                snprintf(script, sizeof(script), "%s done 0 {} 0",
                         ble.on_scan_proc);
                task_send_to_name(ble.on_scan_target, script);
            }
            break;

        case BLE_EVT_CHAR_WRITE:
            if (ble.on_write_proc[0]) {
                /* Encode data as hex */
                char hexdata[513];
                int off = 0;
                for (int i = 0; i < evt.data_len && off < (int)sizeof(hexdata) - 2; i++) {
                    off += snprintf(hexdata + off, sizeof(hexdata) - off, "%02x", evt.data[i]);
                }
                hexdata[off] = '\0';
                snprintf(script, sizeof(script), "%s %d %d %s",
                         ble.on_write_proc, evt.conn_handle, evt.char_handle, hexdata);
                task_send_to_name(ble.on_write_target, script);
            }
            break;

        case BLE_EVT_NOTIFY:
            /* Signal sync_op for blocking read, or deliver callback */
            if (ble.sync_op.sem) {
                ble.sync_op.status = 0;
                memcpy(ble.sync_op.data, evt.data, evt.data_len);
                ble.sync_op.data_len = evt.data_len;
                xSemaphoreGive(ble.sync_op.sem);
            }
            break;

        case BLE_EVT_MTU:
            ESP_LOGI(TAG, "MTU updated: conn=%d mtu=%d", evt.conn_handle, evt.mtu);
            break;

        case BLE_EVT_SUBSCRIBE:
            ESP_LOGD(TAG, "Subscribe event dispatched: conn=%d char=%d",
                     evt.conn_handle, evt.char_handle);
            break;

        case BLE_EVT_PASSKEY:
            if (ble.on_passkey_proc[0]) {
                const char *action_str;
                switch (evt.passkey_action) {
                    case BLE_SM_IOACT_DISP:   action_str = "display"; break;
                    case BLE_SM_IOACT_INPUT:  action_str = "input"; break;
                    case BLE_SM_IOACT_NUMCMP: action_str = "numcmp"; break;
                    case BLE_SM_IOACT_NONE:   action_str = "none"; break;
                    default:                  action_str = "unknown"; break;
                }
                snprintf(script, sizeof(script), "%s %d %s %lu",
                         ble.on_passkey_proc, evt.conn_handle,
                         action_str, (unsigned long)evt.passkey);
                task_send_to_name(ble.on_passkey_target, script);
            }
            break;

        case BLE_EVT_ENC_CHANGE:
            ESP_LOGI(TAG, "Encryption: conn=%d encrypted=%d",
                     evt.conn_handle, evt.encrypted);
            break;
        }
    }
}

/* ---------------------------------------------------------------------------
 * GATT service registration -- builds NimBLE structures from ble_service_t
 * ---------------------------------------------------------------------------*/

static void free_gatt_structs(void)
{
    if (ble.gatt_svcs) {
        free(ble.gatt_svcs);
        ble.gatt_svcs = NULL;
    }
    if (ble.gatt_chr_arrays) {
        for (int i = 0; i < ble.service_count; i++) {
            free(ble.gatt_chr_arrays[i]);
        }
        free(ble.gatt_chr_arrays);
        ble.gatt_chr_arrays = NULL;
    }
    if (ble.gatt_svc_uuids) {
        free(ble.gatt_svc_uuids);
        ble.gatt_svc_uuids = NULL;
    }
    if (ble.gatt_chr_uuids) {
        for (int i = 0; i < ble.service_count; i++) {
            free(ble.gatt_chr_uuids[i]);
        }
        free(ble.gatt_chr_uuids);
        ble.gatt_chr_uuids = NULL;
    }
}

static int build_gatt_svcs(void)
{
    int nsvc = ble.service_count;
    if (nsvc == 0) return 0;

    /* Allocate service defs (nsvc + 1 for null terminator) */
    ble.gatt_svcs = calloc(nsvc + 1, sizeof(struct ble_gatt_svc_def));
    ble.gatt_chr_arrays = calloc(nsvc, sizeof(struct ble_gatt_chr_def *));
    ble.gatt_svc_uuids = calloc(nsvc, sizeof(ble_uuid_any_t));
    ble.gatt_chr_uuids = calloc(nsvc, sizeof(ble_uuid_any_t *));

    if (!ble.gatt_svcs || !ble.gatt_chr_arrays ||
        !ble.gatt_svc_uuids || !ble.gatt_chr_uuids) {
        ESP_LOGE(TAG, "Failed to allocate GATT structures");
        free_gatt_structs();
        return -1;
    }

    for (int s = 0; s < nsvc; s++) {
        ble_service_t *svc = &ble.services[s];
        int nchr = svc->char_count;

        /* Copy service UUID to persistent storage */
        memcpy(&ble.gatt_svc_uuids[s], &svc->uuid, sizeof(ble_uuid_any_t));

        /* Allocate char UUID array */
        ble.gatt_chr_uuids[s] = calloc(nchr, sizeof(ble_uuid_any_t));

        /* Allocate chr_def array (nchr + 1 for null terminator) */
        ble.gatt_chr_arrays[s] = calloc(nchr + 1, sizeof(struct ble_gatt_chr_def));

        if (!ble.gatt_chr_uuids[s] || !ble.gatt_chr_arrays[s]) {
            ESP_LOGE(TAG, "Failed to allocate GATT char structures");
            free_gatt_structs();
            return -1;
        }

        for (int c = 0; c < nchr; c++) {
            ble_char_t *chr = &svc->chars[c];

            /* Copy char UUID to persistent storage */
            memcpy(&ble.gatt_chr_uuids[s][c], &chr->uuid, sizeof(ble_uuid_any_t));

            struct ble_gatt_chr_def *cdef = &ble.gatt_chr_arrays[s][c];
            cdef->uuid = &ble.gatt_chr_uuids[s][c].u;
            cdef->access_cb = gatt_access_cb;
            cdef->arg = (void *)(intptr_t)(s * BLE_MAX_CHARS + c);
            cdef->flags = chr->props;
            cdef->val_handle = &chr->handle;
        }
        /* Null terminator already set by calloc */

        struct ble_gatt_svc_def *sdef = &ble.gatt_svcs[s];
        sdef->type = BLE_GATT_SVC_TYPE_PRIMARY;
        sdef->uuid = &ble.gatt_svc_uuids[s].u;
        sdef->characteristics = ble.gatt_chr_arrays[s];
    }
    /* Null terminator for services array already set by calloc */

    return 0;
}

/* ---------------------------------------------------------------------------
 * Subcommand: ble init
 * ---------------------------------------------------------------------------*/

static int ble_cmd_init(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    const char *name = "JimTcl-BLE";

    /* Security defaults: Just Works, no bonding */
    ble.sm_bonding = 0;
    ble.sm_mitm = 0;
    ble.sm_sc = 1;  /* Secure Connections enabled by default */
    ble.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble.sm_passkey = 0;

    for (int i = 0; i < argc; i++) {
        const char *opt = Jim_String(argv[i]);
        if (strcmp(opt, "-name") == 0 && i + 1 < argc) {
            name = Jim_String(argv[++i]);
        }
        else if (strcmp(opt, "-security") == 0 && i + 1 < argc) {
            /* Parse security dict: {mode just_works|passkey|numcmp bonding 0|1 pin NNNNNN} */
            Jim_Obj *secDict = argv[++i];
            int secLen = Jim_ListLength(interp, secDict);
            for (int j = 0; j < secLen - 1; j += 2) {
                const char *skey = Jim_String(Jim_ListGetIndex(interp, secDict, j));
                Jim_Obj *sval = Jim_ListGetIndex(interp, secDict, j + 1);
                if (strcmp(skey, "mode") == 0) {
                    const char *mode = Jim_String(sval);
                    if (strcmp(mode, "just_works") == 0) {
                        ble.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
                        ble.sm_mitm = 0;
                    } else if (strcmp(mode, "passkey") == 0) {
                        ble.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY;
                        ble.sm_mitm = 1;
                    } else if (strcmp(mode, "passkey_input") == 0) {
                        ble.sm_io_cap = BLE_SM_IO_CAP_KEYBOARD_ONLY;
                        ble.sm_mitm = 1;
                    } else if (strcmp(mode, "passkey_both") == 0) {
                        ble.sm_io_cap = BLE_SM_IO_CAP_KEYBOARD_DISP;
                        ble.sm_mitm = 1;
                    } else if (strcmp(mode, "numcmp") == 0) {
                        ble.sm_io_cap = BLE_SM_IO_CAP_KEYBOARD_DISP;
                        ble.sm_mitm = 1;
                    } else {
                        Jim_SetResultFormatted(interp,
                            "bad security mode \"%s\": should be just_works, passkey, passkey_input, passkey_both, or numcmp", mode);
                        return JIM_ERR;
                    }
                } else if (strcmp(skey, "bonding") == 0) {
                    long v;
                    if (Jim_GetLong(interp, sval, &v) != JIM_OK) return JIM_ERR;
                    ble.sm_bonding = (int)v;
                } else if (strcmp(skey, "pin") == 0) {
                    long v;
                    if (Jim_GetLong(interp, sval, &v) != JIM_OK) return JIM_ERR;
                    if (v < 0 || v > 999999) {
                        Jim_SetResultString(interp, "pin must be 0-999999", -1);
                        return JIM_ERR;
                    }
                    ble.sm_passkey = (uint32_t)v;
                }
            }
        }
        else {
            Jim_SetResultFormatted(interp, "unknown option \"%s\"", opt);
            return JIM_ERR;
        }
    }

    if (ble.initialized) {
        Jim_SetResultString(interp, "BLE already initialized", -1);
        return JIM_ERR;
    }

    /* Create event queue */
    ble.event_queue = xQueueCreate(BLE_EVENT_QUEUE_LEN, sizeof(ble_event_t));
    if (!ble.event_queue) {
        Jim_SetResultString(interp, "failed to create BLE event queue", -1);
        return JIM_ERR;
    }

    /* Create sync event group */
    ble.sync_group = xEventGroupCreate();
    if (!ble.sync_group) {
        vQueueDelete(ble.event_queue);
        ble.event_queue = NULL;
        Jim_SetResultString(interp, "failed to create BLE sync group", -1);
        return JIM_ERR;
    }

    /* Initialize NimBLE */
    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        vQueueDelete(ble.event_queue);
        ble.event_queue = NULL;
        vEventGroupDelete(ble.sync_group);
        ble.sync_group = NULL;
        Jim_SetResultFormatted(interp, "nimble_port_init failed: %s", esp_err_to_name(ret));
        return JIM_ERR;
    }

    /* Configure the NimBLE host */
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.gatts_register_cb = gatt_register_cb;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* Security Manager configuration */
    ble_hs_cfg.sm_io_cap = ble.sm_io_cap;
    ble_hs_cfg.sm_bonding = ble.sm_bonding;
    ble_hs_cfg.sm_mitm = ble.sm_mitm;
    ble_hs_cfg.sm_sc = ble.sm_sc;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    /* Initialize GAP and GATT services */
    ble_svc_gap_init();
    ble_svc_gatt_init();

    /* Register any GATT services that have been added */
    if (ble.service_count > 0) {
        if (build_gatt_svcs() != 0) {
            nimble_port_deinit();
            vQueueDelete(ble.event_queue);
            ble.event_queue = NULL;
            vEventGroupDelete(ble.sync_group);
            ble.sync_group = NULL;
            Jim_SetResultString(interp, "failed to build GATT structures", -1);
            return JIM_ERR;
        }

        int rc = ble_gatts_count_cfg(ble.gatt_svcs);
        if (rc != 0) {
            free_gatt_structs();
            nimble_port_deinit();
            vQueueDelete(ble.event_queue);
            ble.event_queue = NULL;
            vEventGroupDelete(ble.sync_group);
            ble.sync_group = NULL;
            Jim_SetResultFormatted(interp, "ble_gatts_count_cfg failed: %d", rc);
            return JIM_ERR;
        }

        rc = ble_gatts_add_svcs(ble.gatt_svcs);
        if (rc != 0) {
            free_gatt_structs();
            nimble_port_deinit();
            vQueueDelete(ble.event_queue);
            ble.event_queue = NULL;
            vEventGroupDelete(ble.sync_group);
            ble.sync_group = NULL;
            Jim_SetResultFormatted(interp, "ble_gatts_add_svcs failed: %d", rc);
            return JIM_ERR;
        }
    }

    /* Set device name */
    strncpy(ble.device_name, name, sizeof(ble.device_name) - 1);
    ble_svc_gap_device_name_set(ble.device_name);

    /* NimBLE store config */
    extern void ble_store_config_init(void);
    ble_store_config_init();

    /* Start the NimBLE host task */
    nimble_port_freertos_init(ble_host_task);

    /* Wait for sync (NimBLE host ready) */
    EventBits_t bits = xEventGroupWaitBits(ble.sync_group, BLE_SYNC_BIT,
                                            pdTRUE, pdTRUE, pdMS_TO_TICKS(5000));
    if (!(bits & BLE_SYNC_BIT)) {
        ESP_LOGW(TAG, "NimBLE sync timeout -- proceeding anyway");
    }

    /* Start the event dispatcher task */
    BaseType_t xret = xTaskCreate(ble_dispatch_task_fn, "ble_dispatch", 4096, NULL, 6,
                                   &ble.dispatch_task);
    if (xret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create BLE dispatch task");
        /* Not fatal -- callbacks just won't be dispatched */
    }

    ble.initialized = 1;
    ESP_LOGI(TAG, "BLE initialized as \"%s\"", ble.device_name);
    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Subcommand: ble deinit
 * ---------------------------------------------------------------------------*/

static int ble_cmd_deinit(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    (void)argc; (void)argv;

    if (!ble.initialized) {
        Jim_SetResultString(interp, "BLE not initialized", -1);
        return JIM_ERR;
    }

    /* Stop advertising and scanning */
    if (ble.advertising) {
        ble_gap_adv_stop();
        ble.advertising = 0;
    }
    if (ble.scanning) {
        ble_gap_disc_cancel();
        ble.scanning = 0;
    }

    /* Disconnect all connections */
    for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
        if (ble.connections[i].connected) {
            ble_gap_terminate(ble.connections[i].conn_handle,
                              BLE_ERR_REM_USER_CONN_TERM);
            ble.connections[i].connected = 0;
        }
    }

    /* Stop dispatcher task */
    if (ble.dispatch_task) {
        vTaskDelete(ble.dispatch_task);
        ble.dispatch_task = NULL;
    }

    /* Stop NimBLE */
    int rc = nimble_port_stop();
    if (rc == 0) {
        nimble_port_deinit();
    } else {
        ESP_LOGW(TAG, "nimble_port_stop failed: %d", rc);
    }

    /* Clean up */
    if (ble.event_queue) {
        vQueueDelete(ble.event_queue);
        ble.event_queue = NULL;
    }
    if (ble.sync_group) {
        vEventGroupDelete(ble.sync_group);
        ble.sync_group = NULL;
    }

    free_gatt_structs();

    /* Reset state but keep service definitions for re-init */
    ble.initialized = 0;
    ble.advertising = 0;
    ble.scanning = 0;
    ble.scan_count = 0;
    memset(ble.connections, 0, sizeof(ble.connections));

    ESP_LOGI(TAG, "BLE deinitialized");
    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Subcommand: ble address
 * ---------------------------------------------------------------------------*/

static int ble_cmd_address(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    (void)argc; (void)argv;

    if (!ble.initialized) {
        Jim_SetResultString(interp, "BLE not initialized", -1);
        return JIM_ERR;
    }

    uint8_t addr[6] = {0};
    ble_hs_id_copy_addr(ble.own_addr_type, addr, NULL);

    char addr_str[18];
    format_addr(addr, addr_str);
    Jim_SetResultString(interp, addr_str, -1);
    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Subcommand: ble service add|list
 * ---------------------------------------------------------------------------*/

static int ble_cmd_service(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc < 1) {
        Jim_SetResultString(interp,
            "wrong # args: should be \"ble service add|list ...\"", -1);
        return JIM_ERR;
    }

    const char *subcmd = Jim_String(argv[0]);

    /* ble service list */
    if (strcmp(subcmd, "list") == 0) {
        Jim_Obj *listObj = Jim_NewListObj(interp, NULL, 0);
        for (int s = 0; s < ble.service_count; s++) {
            if (!ble.services[s].active) continue;
            Jim_Obj *svcDict = Jim_NewDictObj(interp, NULL, 0);

            char uuid_str[40];
            uuid_to_str(&ble.services[s].uuid, uuid_str, sizeof(uuid_str));
            Jim_DictAddElement(interp, svcDict,
                Jim_NewStringObj(interp, "uuid", -1),
                Jim_NewStringObj(interp, uuid_str, -1));

            Jim_Obj *charList = Jim_NewListObj(interp, NULL, 0);
            for (int c = 0; c < ble.services[s].char_count; c++) {
                ble_char_t *chr = &ble.services[s].chars[c];
                Jim_Obj *chrDict = Jim_NewDictObj(interp, NULL, 0);

                char chr_uuid[40];
                uuid_to_str(&chr->uuid, chr_uuid, sizeof(chr_uuid));
                Jim_DictAddElement(interp, chrDict,
                    Jim_NewStringObj(interp, "uuid", -1),
                    Jim_NewStringObj(interp, chr_uuid, -1));
                Jim_DictAddElement(interp, chrDict,
                    Jim_NewStringObj(interp, "handle", -1),
                    Jim_NewIntObj(interp, chr->handle));
                Jim_DictAddElement(interp, chrDict,
                    Jim_NewStringObj(interp, "value_len", -1),
                    Jim_NewIntObj(interp, chr->value_len));

                Jim_ListAppendElement(interp, charList, chrDict);
            }

            Jim_DictAddElement(interp, svcDict,
                Jim_NewStringObj(interp, "chars", -1), charList);
            Jim_ListAppendElement(interp, listObj, svcDict);
        }
        Jim_SetResult(interp, listObj);
        return JIM_OK;
    }

    /* ble service add {uuid <UUID> chars {{uuid <UUID> props {read write ...} ?value <val>?} ...}} */
    if (strcmp(subcmd, "add") == 0) {
        if (ble.initialized) {
            Jim_SetResultString(interp,
                "services must be added before 'ble init' (NimBLE requires pre-registration)", -1);
            return JIM_ERR;
        }

        if (argc < 2) {
            Jim_SetResultString(interp,
                "wrong # args: should be \"ble service add {uuid ... chars ...}\"", -1);
            return JIM_ERR;
        }

        if (ble.service_count >= BLE_MAX_SERVICES) {
            Jim_SetResultFormatted(interp, "max services (%d) reached", BLE_MAX_SERVICES);
            return JIM_ERR;
        }

        Jim_Obj *svcDict = argv[1];

        /* Get service UUID */
        Jim_Obj *uuidObj = NULL;
        if (Jim_DictKey(interp, svcDict, Jim_NewStringObj(interp, "uuid", -1),
                        &uuidObj, 0) != JIM_OK || !uuidObj) {
            Jim_SetResultString(interp, "service dict must have 'uuid' key", -1);
            return JIM_ERR;
        }

        int svc_idx = ble.service_count;
        ble_service_t *svc = &ble.services[svc_idx];
        memset(svc, 0, sizeof(*svc));

        if (parse_uuid(Jim_String(uuidObj), &svc->uuid) != 0) {
            Jim_SetResultFormatted(interp, "invalid service UUID: %s", Jim_String(uuidObj));
            return JIM_ERR;
        }

        /* Get chars list */
        Jim_Obj *charsObj = NULL;
        if (Jim_DictKey(interp, svcDict, Jim_NewStringObj(interp, "chars", -1),
                        &charsObj, 0) != JIM_OK || !charsObj) {
            Jim_SetResultString(interp, "service dict must have 'chars' key", -1);
            return JIM_ERR;
        }

        int nchars = Jim_ListLength(interp, charsObj);
        if (nchars > BLE_MAX_CHARS) {
            Jim_SetResultFormatted(interp, "max chars per service (%d) exceeded", BLE_MAX_CHARS);
            return JIM_ERR;
        }

        for (int c = 0; c < nchars; c++) {
            Jim_Obj *chrDict = Jim_ListGetIndex(interp, charsObj, c);
            ble_char_t *chr = &svc->chars[c];
            memset(chr, 0, sizeof(*chr));

            /* UUID */
            Jim_Obj *chrUuid = NULL;
            if (Jim_DictKey(interp, chrDict, Jim_NewStringObj(interp, "uuid", -1),
                            &chrUuid, 0) != JIM_OK || !chrUuid) {
                Jim_SetResultString(interp, "char dict must have 'uuid' key", -1);
                return JIM_ERR;
            }
            if (parse_uuid(Jim_String(chrUuid), &chr->uuid) != 0) {
                Jim_SetResultFormatted(interp, "invalid char UUID: %s", Jim_String(chrUuid));
                return JIM_ERR;
            }

            /* Props */
            Jim_Obj *propsObj = NULL;
            if (Jim_DictKey(interp, chrDict, Jim_NewStringObj(interp, "props", -1),
                            &propsObj, 0) == JIM_OK && propsObj) {
                chr->props = parse_chr_flags(interp, propsObj);
            } else {
                chr->props = BLE_GATT_CHR_F_READ;  /* default: read-only */
            }

            /* Initial value (optional) */
            Jim_Obj *valObj = NULL;
            if (Jim_DictKey(interp, chrDict, Jim_NewStringObj(interp, "value", -1),
                            &valObj, 0) == JIM_OK && valObj) {
                int vlen;
                const char *vstr = Jim_GetString(valObj, &vlen);
                if (vlen > (int)sizeof(chr->value)) vlen = sizeof(chr->value);
                memcpy(chr->value, vstr, vlen);
                chr->value_len = vlen;
            }

            svc->char_count++;
        }

        svc->active = 1;
        ble.service_count++;

        Jim_SetResultInt(interp, svc_idx);
        ESP_LOGI(TAG, "Service added (index %d) with %d characteristics", svc_idx, svc->char_count);
        return JIM_OK;
    }

    Jim_SetResultFormatted(interp, "unknown service subcommand \"%s\": should be add or list",
                           subcmd);
    return JIM_ERR;
}

/* ---------------------------------------------------------------------------
 * Subcommand: ble advertise start|stop
 * ---------------------------------------------------------------------------*/

static int ble_cmd_advertise(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc < 1) {
        Jim_SetResultString(interp,
            "wrong # args: should be \"ble advertise start|stop ...\"", -1);
        return JIM_ERR;
    }

    if (!ble.initialized) {
        Jim_SetResultString(interp, "BLE not initialized", -1);
        return JIM_ERR;
    }

    const char *subcmd = Jim_String(argv[0]);

    if (strcmp(subcmd, "stop") == 0) {
        if (ble.advertising) {
            ble_gap_adv_stop();
            ble.advertising = 0;
        }
        return JIM_OK;
    }

    if (strcmp(subcmd, "start") == 0) {
        if (ble.advertising) {
            Jim_SetResultString(interp, "already advertising", -1);
            return JIM_ERR;
        }

        const char *adv_name = ble.device_name;
        uint16_t adv_uuid16 = 0;
        int has_uuid16 = 0;

        for (int i = 1; i < argc; i++) {
            const char *opt = Jim_String(argv[i]);
            if (strcmp(opt, "-name") == 0 && i + 1 < argc) {
                adv_name = Jim_String(argv[++i]);
            } else if (strcmp(opt, "-uuid") == 0 && i + 1 < argc) {
                adv_uuid16 = (uint16_t)strtoul(Jim_String(argv[++i]), NULL, 16);
                has_uuid16 = 1;
            } else {
                Jim_SetResultFormatted(interp, "unknown option \"%s\"", opt);
                return JIM_ERR;
            }
        }

        struct ble_hs_adv_fields fields;
        memset(&fields, 0, sizeof(fields));

        fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
        fields.tx_pwr_lvl_is_present = 1;
        fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

        fields.name = (uint8_t *)adv_name;
        fields.name_len = strlen(adv_name);
        fields.name_is_complete = 1;

        ble_uuid16_t uuid16_val;
        if (has_uuid16) {
            uuid16_val.u.type = BLE_UUID_TYPE_16;
            uuid16_val.value = adv_uuid16;
            fields.uuids16 = &uuid16_val;
            fields.num_uuids16 = 1;
            fields.uuids16_is_complete = 1;
        }

        int rc = ble_gap_adv_set_fields(&fields);
        if (rc != 0) {
            Jim_SetResultFormatted(interp, "ble_gap_adv_set_fields failed: %d", rc);
            return JIM_ERR;
        }

        struct ble_gap_adv_params adv_params;
        memset(&adv_params, 0, sizeof(adv_params));
        adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
        adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

        rc = ble_gap_adv_start(ble.own_addr_type, NULL, BLE_HS_FOREVER,
                                &adv_params, ble_gap_event_cb, NULL);
        if (rc != 0) {
            Jim_SetResultFormatted(interp, "ble_gap_adv_start failed: %d", rc);
            return JIM_ERR;
        }

        ble.advertising = 1;
        ESP_LOGI(TAG, "Advertising started as \"%s\"", adv_name);
        return JIM_OK;
    }

    Jim_SetResultFormatted(interp, "unknown advertise subcommand \"%s\": should be start or stop",
                           subcmd);
    return JIM_ERR;
}

/* ---------------------------------------------------------------------------
 * Subcommand: ble char set|get
 * ---------------------------------------------------------------------------*/

static int ble_cmd_char(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc < 2) {
        Jim_SetResultString(interp,
            "wrong # args: should be \"ble char set|get handle ?value?\"", -1);
        return JIM_ERR;
    }

    if (!ble.initialized) {
        Jim_SetResultString(interp, "BLE not initialized", -1);
        return JIM_ERR;
    }

    const char *subcmd = Jim_String(argv[0]);
    long handle;
    if (Jim_GetLong(interp, argv[1], &handle) != JIM_OK) return JIM_ERR;

    ble_char_t *chr = find_char_by_handle((uint16_t)handle);
    if (!chr) {
        Jim_SetResultFormatted(interp, "characteristic handle %d not found", (int)handle);
        return JIM_ERR;
    }

    if (strcmp(subcmd, "get") == 0) {
        Jim_SetResultString(interp, (const char *)chr->value, chr->value_len);
        return JIM_OK;
    }

    if (strcmp(subcmd, "set") == 0) {
        if (argc < 3) {
            Jim_SetResultString(interp,
                "wrong # args: should be \"ble char set handle value\"", -1);
            return JIM_ERR;
        }
        int vlen;
        const char *vstr = Jim_GetString(argv[2], &vlen);
        if (vlen > (int)sizeof(chr->value)) vlen = sizeof(chr->value);
        memcpy(chr->value, vstr, vlen);
        chr->value_len = vlen;
        return JIM_OK;
    }

    Jim_SetResultFormatted(interp, "unknown char subcommand \"%s\": should be set or get",
                           subcmd);
    return JIM_ERR;
}

/* ---------------------------------------------------------------------------
 * Subcommand: ble notify <handle> ?-conn <conn_handle>?
 * ---------------------------------------------------------------------------*/

static int ble_cmd_notify(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc < 1) {
        Jim_SetResultString(interp,
            "wrong # args: should be \"ble notify handle ?-conn conn_handle?\"", -1);
        return JIM_ERR;
    }

    if (!ble.initialized) {
        Jim_SetResultString(interp, "BLE not initialized", -1);
        return JIM_ERR;
    }

    long handle;
    if (Jim_GetLong(interp, argv[0], &handle) != JIM_OK) return JIM_ERR;

    /* Optional: specific connection handle, or notify all */
    long conn_handle = -1;
    for (int i = 1; i < argc; i++) {
        const char *opt = Jim_String(argv[i]);
        if (strcmp(opt, "-conn") == 0 && i + 1 < argc) {
            if (Jim_GetLong(interp, argv[++i], &conn_handle) != JIM_OK) return JIM_ERR;
        }
    }

    /* ble_gatts_chr_updated triggers notifications/indications for all subscribed peers */
    ble_gatts_chr_updated((uint16_t)handle);
    ESP_LOGD(TAG, "Notification triggered for handle %d", (int)handle);

    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Subcommand: ble scan start|stop|results
 * ---------------------------------------------------------------------------*/

static int ble_cmd_scan(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc < 1) {
        Jim_SetResultString(interp,
            "wrong # args: should be \"ble scan start|stop|results ...\"", -1);
        return JIM_ERR;
    }

    if (!ble.initialized) {
        Jim_SetResultString(interp, "BLE not initialized", -1);
        return JIM_ERR;
    }

    const char *subcmd = Jim_String(argv[0]);

    /* ble scan stop */
    if (strcmp(subcmd, "stop") == 0) {
        if (ble.scanning) {
            ble_gap_disc_cancel();
            ble.scanning = 0;
        }
        return JIM_OK;
    }

    /* ble scan results */
    if (strcmp(subcmd, "results") == 0) {
        Jim_Obj *listObj = Jim_NewListObj(interp, NULL, 0);
        for (int i = 0; i < ble.scan_count; i++) {
            ble_scan_result_t *r = &ble.scan_results[i];
            Jim_Obj *d = Jim_NewDictObj(interp, NULL, 0);

            char addr_str[18];
            format_addr(r->addr, addr_str);
            Jim_DictAddElement(interp, d,
                Jim_NewStringObj(interp, "addr", -1),
                Jim_NewStringObj(interp, addr_str, -1));
            Jim_DictAddElement(interp, d,
                Jim_NewStringObj(interp, "rssi", -1),
                Jim_NewIntObj(interp, r->rssi));
            Jim_DictAddElement(interp, d,
                Jim_NewStringObj(interp, "name", -1),
                Jim_NewStringObj(interp, r->name, -1));
            Jim_DictAddElement(interp, d,
                Jim_NewStringObj(interp, "addr_type", -1),
                Jim_NewIntObj(interp, r->addr_type));

            Jim_ListAppendElement(interp, listObj, d);
        }
        Jim_SetResult(interp, listObj);
        return JIM_OK;
    }

    /* ble scan start ?-duration <ms>? ?-active 0|1? ?-callback {proc task}? */
    if (strcmp(subcmd, "start") == 0) {
        if (ble.scanning) {
            Jim_SetResultString(interp, "already scanning", -1);
            return JIM_ERR;
        }

        long duration_ms = 10000;  /* default 10 seconds */
        int active = 0;
        const char *cb_proc = NULL;
        const char *cb_target = NULL;

        for (int i = 1; i < argc; i++) {
            const char *opt = Jim_String(argv[i]);
            if (strcmp(opt, "-duration") == 0 && i + 1 < argc) {
                if (Jim_GetLong(interp, argv[++i], &duration_ms) != JIM_OK) return JIM_ERR;
            } else if (strcmp(opt, "-active") == 0 && i + 1 < argc) {
                long v;
                if (Jim_GetLong(interp, argv[++i], &v) != JIM_OK) return JIM_ERR;
                active = (int)v;
            } else if (strcmp(opt, "-callback") == 0 && i + 1 < argc) {
                Jim_Obj *cbObj = argv[++i];
                if (Jim_ListLength(interp, cbObj) != 2) {
                    Jim_SetResultString(interp, "-callback requires {procname target_task}", -1);
                    return JIM_ERR;
                }
                cb_proc = Jim_String(Jim_ListGetIndex(interp, cbObj, 0));
                cb_target = Jim_String(Jim_ListGetIndex(interp, cbObj, 1));
            } else {
                Jim_SetResultFormatted(interp, "unknown option \"%s\"", opt);
                return JIM_ERR;
            }
        }

        /* Set scan callback if provided */
        if (cb_proc && cb_target) {
            strncpy(ble.on_scan_proc, cb_proc, sizeof(ble.on_scan_proc) - 1);
            ble.on_scan_proc[sizeof(ble.on_scan_proc) - 1] = '\0';
            strncpy(ble.on_scan_target, cb_target, sizeof(ble.on_scan_target) - 1);
            ble.on_scan_target[sizeof(ble.on_scan_target) - 1] = '\0';
        }

        /* Clear previous results */
        ble.scan_count = 0;

        struct ble_gap_disc_params disc_params;
        memset(&disc_params, 0, sizeof(disc_params));
        disc_params.filter_duplicates = 1;
        disc_params.passive = active ? 0 : 1;
        disc_params.itvl = 0;    /* use defaults */
        disc_params.window = 0;
        disc_params.filter_policy = 0;
        disc_params.limited = 0;

        /* NimBLE uses units of 10ms for duration, or BLE_HS_FOREVER */
        int32_t duration_nimble;
        if (duration_ms <= 0) {
            duration_nimble = BLE_HS_FOREVER;
        } else {
            duration_nimble = (int32_t)duration_ms;
        }

        int rc = ble_gap_disc(ble.own_addr_type, duration_nimble,
                               &disc_params, ble_gap_event_cb, NULL);
        if (rc != 0) {
            Jim_SetResultFormatted(interp, "ble_gap_disc failed: %d", rc);
            return JIM_ERR;
        }

        ble.scanning = 1;
        ESP_LOGI(TAG, "Scan started (duration=%ldms active=%d)", duration_ms, active);
        return JIM_OK;
    }

    Jim_SetResultFormatted(interp, "unknown scan subcommand \"%s\": should be start, stop, or results",
                           subcmd);
    return JIM_ERR;
}

/* ---------------------------------------------------------------------------
 * Subcommand: ble connect <addr> ?-addr_type public|random? ?-timeout <ms>?
 * ---------------------------------------------------------------------------*/

static int ble_cmd_connect(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc < 1) {
        Jim_SetResultString(interp,
            "wrong # args: should be \"ble connect addr ?-addr_type public|random? ?-timeout ms?\"", -1);
        return JIM_ERR;
    }

    if (!ble.initialized) {
        Jim_SetResultString(interp, "BLE not initialized", -1);
        return JIM_ERR;
    }

    uint8_t peer_addr[6];
    if (parse_addr(Jim_String(argv[0]), peer_addr) != 0) {
        Jim_SetResultFormatted(interp, "invalid address: %s", Jim_String(argv[0]));
        return JIM_ERR;
    }

    uint8_t addr_type = BLE_ADDR_PUBLIC;
    long timeout_ms = 10000;

    for (int i = 1; i < argc; i++) {
        const char *opt = Jim_String(argv[i]);
        if (strcmp(opt, "-addr_type") == 0 && i + 1 < argc) {
            const char *at = Jim_String(argv[++i]);
            if (strcmp(at, "random") == 0) {
                addr_type = BLE_ADDR_RANDOM;
            } else {
                addr_type = BLE_ADDR_PUBLIC;
            }
        } else if (strcmp(opt, "-timeout") == 0 && i + 1 < argc) {
            if (Jim_GetLong(interp, argv[++i], &timeout_ms) != JIM_OK) return JIM_ERR;
        } else {
            Jim_SetResultFormatted(interp, "unknown option \"%s\"", opt);
            return JIM_ERR;
        }
    }

    /* Stop scanning if active (required before connecting) */
    if (ble.scanning) {
        ble_gap_disc_cancel();
        ble.scanning = 0;
    }

    /* Create sync semaphore for blocking connect */
    ble.sync_op.sem = xSemaphoreCreateBinary();
    if (!ble.sync_op.sem) {
        Jim_SetResultString(interp, "failed to create sync semaphore", -1);
        return JIM_ERR;
    }
    ble.sync_op.status = -1;

    ble_addr_t ba;
    ba.type = addr_type;
    memcpy(ba.val, peer_addr, 6);

    int rc = ble_gap_connect(ble.own_addr_type, &ba, (int32_t)timeout_ms,
                              NULL, ble_gap_event_cb, NULL);
    if (rc != 0) {
        vSemaphoreDelete(ble.sync_op.sem);
        ble.sync_op.sem = NULL;
        Jim_SetResultFormatted(interp, "ble_gap_connect failed: %d", rc);
        return JIM_ERR;
    }

    /* Wait for connect event */
    if (xSemaphoreTake(ble.sync_op.sem, pdMS_TO_TICKS(timeout_ms + 1000)) != pdTRUE) {
        vSemaphoreDelete(ble.sync_op.sem);
        ble.sync_op.sem = NULL;
        Jim_SetResultString(interp, "connect timeout", -1);
        return JIM_ERR;
    }

    int status = ble.sync_op.status;
    uint16_t conn_handle = 0;
    if (status == 0 && ble.sync_op.data_len >= 2) {
        conn_handle = (uint16_t)(ble.sync_op.data[0] | (ble.sync_op.data[1] << 8));
    }

    vSemaphoreDelete(ble.sync_op.sem);
    ble.sync_op.sem = NULL;

    if (status != 0) {
        Jim_SetResultFormatted(interp, "connect failed: status=%d", status);
        return JIM_ERR;
    }

    Jim_SetResultInt(interp, conn_handle);
    ESP_LOGI(TAG, "Connected: conn_handle=%d", conn_handle);
    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Subcommand: ble disconnect <conn_handle>
 * ---------------------------------------------------------------------------*/

static int ble_cmd_disconnect(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc < 1) {
        Jim_SetResultString(interp,
            "wrong # args: should be \"ble disconnect conn_handle\"", -1);
        return JIM_ERR;
    }

    if (!ble.initialized) {
        Jim_SetResultString(interp, "BLE not initialized", -1);
        return JIM_ERR;
    }

    long conn_handle;
    if (Jim_GetLong(interp, argv[0], &conn_handle) != JIM_OK) return JIM_ERR;

    int rc = ble_gap_terminate((uint16_t)conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    if (rc != 0) {
        Jim_SetResultFormatted(interp, "ble_gap_terminate failed: %d", rc);
        return JIM_ERR;
    }

    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * GATTC discovery callback types and helpers
 * ---------------------------------------------------------------------------*/

typedef struct {
    Jim_Obj *result;     /* will be populated by discover callback */
    Jim_Interp *interp;
    SemaphoreHandle_t sem;
    int status;
    /* Discovered services */
    struct {
        ble_uuid_any_t uuid;
        uint16_t start_handle;
        uint16_t end_handle;
    } svcs[16];
    int svc_count;
    /* Discovered characteristics */
    struct {
        ble_uuid_any_t uuid;
        uint16_t def_handle;
        uint16_t val_handle;
        uint8_t properties;
    } chrs[32];
    int chr_count;
} ble_disc_state_t;

static ble_disc_state_t disc_state;

static int disc_svc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                        const struct ble_gatt_svc *service, void *arg)
{
    if (error->status == 0 && service) {
        if (disc_state.svc_count < 16) {
            int idx = disc_state.svc_count++;
            ble_uuid_copy((ble_uuid_t *)&disc_state.svcs[idx].uuid, &service->uuid.u);
            disc_state.svcs[idx].start_handle = service->start_handle;
            disc_state.svcs[idx].end_handle = service->end_handle;
        }
    } else if (error->status == BLE_HS_EDONE) {
        disc_state.status = 0;
        xSemaphoreGive(disc_state.sem);
    } else {
        disc_state.status = error->status;
        xSemaphoreGive(disc_state.sem);
    }
    return 0;
}

static int disc_chr_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                        const struct ble_gatt_chr *chr, void *arg)
{
    if (error->status == 0 && chr) {
        if (disc_state.chr_count < 32) {
            int idx = disc_state.chr_count++;
            ble_uuid_copy((ble_uuid_t *)&disc_state.chrs[idx].uuid, &chr->uuid.u);
            disc_state.chrs[idx].def_handle = chr->def_handle;
            disc_state.chrs[idx].val_handle = chr->val_handle;
            disc_state.chrs[idx].properties = chr->properties;
        }
    } else if (error->status == BLE_HS_EDONE) {
        disc_state.status = 0;
        xSemaphoreGive(disc_state.sem);
    } else {
        disc_state.status = error->status;
        xSemaphoreGive(disc_state.sem);
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Subcommand: ble discover <conn_handle>
 * ---------------------------------------------------------------------------*/

static int ble_cmd_discover(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc < 1) {
        Jim_SetResultString(interp,
            "wrong # args: should be \"ble discover conn_handle\"", -1);
        return JIM_ERR;
    }

    if (!ble.initialized) {
        Jim_SetResultString(interp, "BLE not initialized", -1);
        return JIM_ERR;
    }

    long conn_handle;
    if (Jim_GetLong(interp, argv[0], &conn_handle) != JIM_OK) return JIM_ERR;

    disc_state.sem = xSemaphoreCreateBinary();
    if (!disc_state.sem) {
        Jim_SetResultString(interp, "failed to create semaphore", -1);
        return JIM_ERR;
    }

    /* Discover all services */
    disc_state.svc_count = 0;
    disc_state.status = -1;

    int rc = ble_gattc_disc_all_svcs((uint16_t)conn_handle, disc_svc_cb, NULL);
    if (rc != 0) {
        vSemaphoreDelete(disc_state.sem);
        Jim_SetResultFormatted(interp, "ble_gattc_disc_all_svcs failed: %d", rc);
        return JIM_ERR;
    }

    if (xSemaphoreTake(disc_state.sem, pdMS_TO_TICKS(10000)) != pdTRUE) {
        vSemaphoreDelete(disc_state.sem);
        Jim_SetResultString(interp, "service discovery timeout", -1);
        return JIM_ERR;
    }

    if (disc_state.status != 0) {
        vSemaphoreDelete(disc_state.sem);
        Jim_SetResultFormatted(interp, "service discovery failed: %d", disc_state.status);
        return JIM_ERR;
    }

    /* Build result: list of services, each with their characteristics */
    Jim_Obj *resultList = Jim_NewListObj(interp, NULL, 0);

    for (int s = 0; s < disc_state.svc_count; s++) {
        /* Discover chars for this service */
        disc_state.chr_count = 0;
        disc_state.status = -1;

        rc = ble_gattc_disc_all_chrs((uint16_t)conn_handle,
                                      disc_state.svcs[s].start_handle,
                                      disc_state.svcs[s].end_handle,
                                      disc_chr_cb, NULL);
        if (rc != 0) {
            ESP_LOGW(TAG, "disc_all_chrs failed for svc %d: %d", s, rc);
            continue;
        }

        if (xSemaphoreTake(disc_state.sem, pdMS_TO_TICKS(10000)) != pdTRUE) {
            ESP_LOGW(TAG, "char discovery timeout for svc %d", s);
            continue;
        }

        Jim_Obj *svcDict = Jim_NewDictObj(interp, NULL, 0);

        char uuid_str[40];
        ble_uuid_to_str(&disc_state.svcs[s].uuid.u, uuid_str);
        Jim_DictAddElement(interp, svcDict,
            Jim_NewStringObj(interp, "uuid", -1),
            Jim_NewStringObj(interp, uuid_str, -1));
        Jim_DictAddElement(interp, svcDict,
            Jim_NewStringObj(interp, "start_handle", -1),
            Jim_NewIntObj(interp, disc_state.svcs[s].start_handle));
        Jim_DictAddElement(interp, svcDict,
            Jim_NewStringObj(interp, "end_handle", -1),
            Jim_NewIntObj(interp, disc_state.svcs[s].end_handle));

        Jim_Obj *charList = Jim_NewListObj(interp, NULL, 0);
        for (int c = 0; c < disc_state.chr_count; c++) {
            Jim_Obj *chrDict = Jim_NewDictObj(interp, NULL, 0);

            char chr_uuid_str[40];
            ble_uuid_to_str(&disc_state.chrs[c].uuid.u, chr_uuid_str);
            Jim_DictAddElement(interp, chrDict,
                Jim_NewStringObj(interp, "uuid", -1),
                Jim_NewStringObj(interp, chr_uuid_str, -1));
            Jim_DictAddElement(interp, chrDict,
                Jim_NewStringObj(interp, "def_handle", -1),
                Jim_NewIntObj(interp, disc_state.chrs[c].def_handle));
            Jim_DictAddElement(interp, chrDict,
                Jim_NewStringObj(interp, "val_handle", -1),
                Jim_NewIntObj(interp, disc_state.chrs[c].val_handle));
            Jim_DictAddElement(interp, chrDict,
                Jim_NewStringObj(interp, "properties", -1),
                Jim_NewIntObj(interp, disc_state.chrs[c].properties));

            Jim_ListAppendElement(interp, charList, chrDict);
        }

        Jim_DictAddElement(interp, svcDict,
            Jim_NewStringObj(interp, "chars", -1), charList);
        Jim_ListAppendElement(interp, resultList, svcDict);
    }

    vSemaphoreDelete(disc_state.sem);
    disc_state.sem = NULL;

    Jim_SetResult(interp, resultList);
    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * GATTC read/write callbacks
 * ---------------------------------------------------------------------------*/

static ble_sync_op_t gattc_op;

static int gattc_read_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                          struct ble_gatt_attr *attr, void *arg)
{
    if (error->status == 0 && attr && attr->om) {
        uint16_t om_len = OS_MBUF_PKTLEN(attr->om);
        if (om_len > sizeof(gattc_op.data)) om_len = sizeof(gattc_op.data);
        ble_hs_mbuf_to_flat(attr->om, gattc_op.data, sizeof(gattc_op.data), NULL);
        gattc_op.data_len = om_len;
        gattc_op.status = 0;
    } else {
        gattc_op.status = error->status;
        gattc_op.data_len = 0;
    }
    xSemaphoreGive(gattc_op.sem);
    return 0;
}

static int gattc_write_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                           struct ble_gatt_attr *attr, void *arg)
{
    gattc_op.status = error->status;
    xSemaphoreGive(gattc_op.sem);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Subcommand: ble read <conn_handle> <attr_handle>
 * ---------------------------------------------------------------------------*/

static int ble_cmd_read(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc < 2) {
        Jim_SetResultString(interp,
            "wrong # args: should be \"ble read conn_handle attr_handle\"", -1);
        return JIM_ERR;
    }

    if (!ble.initialized) {
        Jim_SetResultString(interp, "BLE not initialized", -1);
        return JIM_ERR;
    }

    long conn_handle, attr_handle;
    if (Jim_GetLong(interp, argv[0], &conn_handle) != JIM_OK) return JIM_ERR;
    if (Jim_GetLong(interp, argv[1], &attr_handle) != JIM_OK) return JIM_ERR;

    gattc_op.sem = xSemaphoreCreateBinary();
    if (!gattc_op.sem) {
        Jim_SetResultString(interp, "failed to create semaphore", -1);
        return JIM_ERR;
    }
    gattc_op.status = -1;
    gattc_op.data_len = 0;

    int rc = ble_gattc_read((uint16_t)conn_handle, (uint16_t)attr_handle,
                             gattc_read_cb, NULL);
    if (rc != 0) {
        vSemaphoreDelete(gattc_op.sem);
        gattc_op.sem = NULL;
        Jim_SetResultFormatted(interp, "ble_gattc_read failed: %d", rc);
        return JIM_ERR;
    }

    if (xSemaphoreTake(gattc_op.sem, pdMS_TO_TICKS(10000)) != pdTRUE) {
        vSemaphoreDelete(gattc_op.sem);
        gattc_op.sem = NULL;
        Jim_SetResultString(interp, "read timeout", -1);
        return JIM_ERR;
    }

    vSemaphoreDelete(gattc_op.sem);
    gattc_op.sem = NULL;

    if (gattc_op.status != 0) {
        Jim_SetResultFormatted(interp, "read failed: status=%d", gattc_op.status);
        return JIM_ERR;
    }

    Jim_SetResultString(interp, (const char *)gattc_op.data, gattc_op.data_len);
    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Subcommand: ble write <conn_handle> <attr_handle> <data>
 * ---------------------------------------------------------------------------*/

static int ble_cmd_write(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc < 3) {
        Jim_SetResultString(interp,
            "wrong # args: should be \"ble write conn_handle attr_handle data\"", -1);
        return JIM_ERR;
    }

    if (!ble.initialized) {
        Jim_SetResultString(interp, "BLE not initialized", -1);
        return JIM_ERR;
    }

    long conn_handle, attr_handle;
    if (Jim_GetLong(interp, argv[0], &conn_handle) != JIM_OK) return JIM_ERR;
    if (Jim_GetLong(interp, argv[1], &attr_handle) != JIM_OK) return JIM_ERR;

    int data_len;
    const char *data = Jim_GetString(argv[2], &data_len);

    gattc_op.sem = xSemaphoreCreateBinary();
    if (!gattc_op.sem) {
        Jim_SetResultString(interp, "failed to create semaphore", -1);
        return JIM_ERR;
    }
    gattc_op.status = -1;

    int rc = ble_gattc_write_flat((uint16_t)conn_handle, (uint16_t)attr_handle,
                                   data, data_len, gattc_write_cb, NULL);
    if (rc != 0) {
        vSemaphoreDelete(gattc_op.sem);
        gattc_op.sem = NULL;
        Jim_SetResultFormatted(interp, "ble_gattc_write_flat failed: %d", rc);
        return JIM_ERR;
    }

    if (xSemaphoreTake(gattc_op.sem, pdMS_TO_TICKS(10000)) != pdTRUE) {
        vSemaphoreDelete(gattc_op.sem);
        gattc_op.sem = NULL;
        Jim_SetResultString(interp, "write timeout", -1);
        return JIM_ERR;
    }

    vSemaphoreDelete(gattc_op.sem);
    gattc_op.sem = NULL;

    if (gattc_op.status != 0) {
        Jim_SetResultFormatted(interp, "write failed: status=%d", gattc_op.status);
        return JIM_ERR;
    }

    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Subcommand: ble subscribe <conn_handle> <cccd_handle> ?-notify? ?-indicate?
 * ---------------------------------------------------------------------------*/

static int ble_cmd_subscribe(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc < 2) {
        Jim_SetResultString(interp,
            "wrong # args: should be \"ble subscribe conn_handle cccd_handle ?-notify? ?-indicate?\"", -1);
        return JIM_ERR;
    }

    if (!ble.initialized) {
        Jim_SetResultString(interp, "BLE not initialized", -1);
        return JIM_ERR;
    }

    long conn_handle, cccd_handle;
    if (Jim_GetLong(interp, argv[0], &conn_handle) != JIM_OK) return JIM_ERR;
    if (Jim_GetLong(interp, argv[1], &cccd_handle) != JIM_OK) return JIM_ERR;

    uint8_t value[2] = {0, 0};
    for (int i = 2; i < argc; i++) {
        const char *opt = Jim_String(argv[i]);
        if (strcmp(opt, "-notify") == 0) {
            value[0] |= 0x01;
        } else if (strcmp(opt, "-indicate") == 0) {
            value[0] |= 0x02;
        } else {
            Jim_SetResultFormatted(interp, "unknown option \"%s\"", opt);
            return JIM_ERR;
        }
    }

    /* Default to notify if no flags given */
    if (value[0] == 0) {
        value[0] = 0x01;
    }

    gattc_op.sem = xSemaphoreCreateBinary();
    if (!gattc_op.sem) {
        Jim_SetResultString(interp, "failed to create semaphore", -1);
        return JIM_ERR;
    }
    gattc_op.status = -1;

    int rc = ble_gattc_write_flat((uint16_t)conn_handle, (uint16_t)cccd_handle,
                                   value, sizeof(value), gattc_write_cb, NULL);
    if (rc != 0) {
        vSemaphoreDelete(gattc_op.sem);
        gattc_op.sem = NULL;
        Jim_SetResultFormatted(interp, "ble_gattc_write_flat (subscribe) failed: %d", rc);
        return JIM_ERR;
    }

    if (xSemaphoreTake(gattc_op.sem, pdMS_TO_TICKS(10000)) != pdTRUE) {
        vSemaphoreDelete(gattc_op.sem);
        gattc_op.sem = NULL;
        Jim_SetResultString(interp, "subscribe timeout", -1);
        return JIM_ERR;
    }

    vSemaphoreDelete(gattc_op.sem);
    gattc_op.sem = NULL;

    if (gattc_op.status != 0) {
        Jim_SetResultFormatted(interp, "subscribe failed: status=%d", gattc_op.status);
        return JIM_ERR;
    }

    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Subcommand: ble status
 * ---------------------------------------------------------------------------*/

static int ble_cmd_status(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    (void)argc; (void)argv;

    Jim_Obj *d = Jim_NewDictObj(interp, NULL, 0);

    Jim_DictAddElement(interp, d,
        Jim_NewStringObj(interp, "initialized", -1),
        Jim_NewIntObj(interp, ble.initialized));
    Jim_DictAddElement(interp, d,
        Jim_NewStringObj(interp, "advertising", -1),
        Jim_NewIntObj(interp, ble.advertising));
    Jim_DictAddElement(interp, d,
        Jim_NewStringObj(interp, "scanning", -1),
        Jim_NewIntObj(interp, ble.scanning));
    Jim_DictAddElement(interp, d,
        Jim_NewStringObj(interp, "service_count", -1),
        Jim_NewIntObj(interp, ble.service_count));

    int conn_count = 0;
    for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
        if (ble.connections[i].connected) conn_count++;
    }
    Jim_DictAddElement(interp, d,
        Jim_NewStringObj(interp, "connections", -1),
        Jim_NewIntObj(interp, conn_count));
    Jim_DictAddElement(interp, d,
        Jim_NewStringObj(interp, "scan_results", -1),
        Jim_NewIntObj(interp, ble.scan_count));

    if (ble.initialized) {
        char addr_str[18];
        uint8_t addr[6] = {0};
        ble_hs_id_copy_addr(ble.own_addr_type, addr, NULL);
        format_addr(addr, addr_str);
        Jim_DictAddElement(interp, d,
            Jim_NewStringObj(interp, "address", -1),
            Jim_NewStringObj(interp, addr_str, -1));
        Jim_DictAddElement(interp, d,
            Jim_NewStringObj(interp, "device_name", -1),
            Jim_NewStringObj(interp, ble.device_name, -1));
    }

    Jim_SetResult(interp, d);
    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Subcommand: ble connections
 * ---------------------------------------------------------------------------*/

static int ble_cmd_connections(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    (void)argc; (void)argv;

    Jim_Obj *listObj = Jim_NewListObj(interp, NULL, 0);
    for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
        if (!ble.connections[i].connected) continue;

        Jim_Obj *d = Jim_NewDictObj(interp, NULL, 0);
        Jim_DictAddElement(interp, d,
            Jim_NewStringObj(interp, "conn_handle", -1),
            Jim_NewIntObj(interp, ble.connections[i].conn_handle));

        char addr_str[18];
        format_addr(ble.connections[i].peer_addr, addr_str);
        Jim_DictAddElement(interp, d,
            Jim_NewStringObj(interp, "addr", -1),
            Jim_NewStringObj(interp, addr_str, -1));
        Jim_DictAddElement(interp, d,
            Jim_NewStringObj(interp, "addr_type", -1),
            Jim_NewIntObj(interp, ble.connections[i].addr_type));

        Jim_ListAppendElement(interp, listObj, d);
    }
    Jim_SetResult(interp, listObj);
    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Subcommand: ble on <event> ?-callback {proc task}?
 * ---------------------------------------------------------------------------*/

static int ble_cmd_on(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc < 1) {
        Jim_SetResultString(interp,
            "wrong # args: should be \"ble on connect|disconnect|scan|write "
            "?-callback {proc task}?\"", -1);
        return JIM_ERR;
    }

    const char *event_name = Jim_String(argv[0]);
    char *proc_buf = NULL;
    char *target_buf = NULL;
    int proc_size = 0, target_size = 0;

    if (strcmp(event_name, "connect") == 0) {
        proc_buf = ble.on_connect_proc;
        target_buf = ble.on_connect_target;
        proc_size = sizeof(ble.on_connect_proc);
        target_size = sizeof(ble.on_connect_target);
    } else if (strcmp(event_name, "disconnect") == 0) {
        proc_buf = ble.on_disconnect_proc;
        target_buf = ble.on_disconnect_target;
        proc_size = sizeof(ble.on_disconnect_proc);
        target_size = sizeof(ble.on_disconnect_target);
    } else if (strcmp(event_name, "scan") == 0) {
        proc_buf = ble.on_scan_proc;
        target_buf = ble.on_scan_target;
        proc_size = sizeof(ble.on_scan_proc);
        target_size = sizeof(ble.on_scan_target);
    } else if (strcmp(event_name, "write") == 0) {
        proc_buf = ble.on_write_proc;
        target_buf = ble.on_write_target;
        proc_size = sizeof(ble.on_write_proc);
        target_size = sizeof(ble.on_write_target);
    } else if (strcmp(event_name, "passkey") == 0) {
        proc_buf = ble.on_passkey_proc;
        target_buf = ble.on_passkey_target;
        proc_size = sizeof(ble.on_passkey_proc);
        target_size = sizeof(ble.on_passkey_target);
    } else {
        Jim_SetResultFormatted(interp, "unknown event \"%s\": should be connect, disconnect, scan, write, or passkey",
                               event_name);
        return JIM_ERR;
    }

    /* If no further args, just show current callback */
    if (argc == 1) {
        if (proc_buf[0]) {
            Jim_Obj *listObj = Jim_NewListObj(interp, NULL, 0);
            Jim_ListAppendElement(interp, listObj, Jim_NewStringObj(interp, proc_buf, -1));
            Jim_ListAppendElement(interp, listObj, Jim_NewStringObj(interp, target_buf, -1));
            Jim_SetResult(interp, listObj);
        } else {
            Jim_SetResultString(interp, "", 0);
        }
        return JIM_OK;
    }

    /* Parse -callback {proc task} or -remove */
    for (int i = 1; i < argc; i++) {
        const char *opt = Jim_String(argv[i]);
        if (strcmp(opt, "-callback") == 0 && i + 1 < argc) {
            Jim_Obj *cbObj = argv[++i];
            if (Jim_ListLength(interp, cbObj) != 2) {
                Jim_SetResultString(interp, "-callback requires {procname target_task}", -1);
                return JIM_ERR;
            }
            const char *proc = Jim_String(Jim_ListGetIndex(interp, cbObj, 0));
            const char *target = Jim_String(Jim_ListGetIndex(interp, cbObj, 1));
            strncpy(proc_buf, proc, proc_size - 1);
            proc_buf[proc_size - 1] = '\0';
            strncpy(target_buf, target, target_size - 1);
            target_buf[target_size - 1] = '\0';
        } else if (strcmp(opt, "-remove") == 0) {
            proc_buf[0] = '\0';
            target_buf[0] = '\0';
        } else {
            Jim_SetResultFormatted(interp, "unknown option \"%s\"", opt);
            return JIM_ERR;
        }
    }

    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Subcommand table and init
 * ---------------------------------------------------------------------------*/

/* ---------------------------------------------------------------------------
 * Subcommand: ble bonds list|clear|remove <addr>
 * ---------------------------------------------------------------------------*/

static int ble_cmd_bonds(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc < 1) {
        Jim_SetResultString(interp,
            "wrong # args: should be \"ble bonds list|clear|remove <addr>\"", -1);
        return JIM_ERR;
    }

    const char *subcmd = Jim_String(argv[0]);

    if (strcmp(subcmd, "list") == 0) {
        ble_addr_t peer_addrs[MYNEWT_VAL(BLE_STORE_MAX_BONDS)];
        int num_peers = 0;
        int rc = ble_store_util_bonded_peers(peer_addrs, &num_peers,
                     MYNEWT_VAL(BLE_STORE_MAX_BONDS));
        if (rc != 0) {
            Jim_SetResultFormatted(interp, "failed to enumerate bonds: %d", rc);
            return JIM_ERR;
        }

        Jim_Obj *result = Jim_NewListObj(interp, NULL, 0);
        for (int i = 0; i < num_peers; i++) {
            char addr_str[18];
            snprintf(addr_str, sizeof(addr_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                     peer_addrs[i].val[5], peer_addrs[i].val[4],
                     peer_addrs[i].val[3], peer_addrs[i].val[2],
                     peer_addrs[i].val[1], peer_addrs[i].val[0]);

            Jim_Obj *entry = Jim_NewListObj(interp, NULL, 0);
            Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, "addr", -1));
            Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, addr_str, -1));
            Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, "addr_type", -1));
            Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp,
                peer_addrs[i].type == BLE_ADDR_PUBLIC ? "public" : "random", -1));
            Jim_ListAppendElement(interp, result, entry);
        }
        Jim_SetResult(interp, result);
        return JIM_OK;
    }

    if (strcmp(subcmd, "clear") == 0) {
        int rc = ble_store_clear();
        if (rc != 0) {
            Jim_SetResultFormatted(interp, "failed to clear bonds: %d", rc);
            return JIM_ERR;
        }
        Jim_SetResultString(interp, "ok", -1);
        return JIM_OK;
    }

    if (strcmp(subcmd, "remove") == 0) {
        if (argc < 2) {
            Jim_SetResultString(interp, "missing address for bonds remove", -1);
            return JIM_ERR;
        }
        const char *addr_str = Jim_String(argv[1]);
        ble_addr_t addr;
        /* Parse MAC address */
        unsigned int m[6];
        if (sscanf(addr_str, "%02x:%02x:%02x:%02x:%02x:%02x",
                   &m[5], &m[4], &m[3], &m[2], &m[1], &m[0]) != 6 &&
            sscanf(addr_str, "%02X:%02X:%02X:%02X:%02X:%02X",
                   &m[5], &m[4], &m[3], &m[2], &m[1], &m[0]) != 6) {
            Jim_SetResultFormatted(interp, "invalid MAC address: %s", addr_str);
            return JIM_ERR;
        }
        for (int i = 0; i < 6; i++) addr.val[i] = (uint8_t)m[i];

        /* Try both public and random address types */
        addr.type = BLE_ADDR_PUBLIC;
        int rc = ble_store_util_delete_peer(&addr);
        if (rc != 0) {
            addr.type = BLE_ADDR_RANDOM;
            rc = ble_store_util_delete_peer(&addr);
        }
        if (rc != 0) {
            Jim_SetResultFormatted(interp, "bond not found for %s", addr_str);
            return JIM_ERR;
        }
        Jim_SetResultString(interp, "ok", -1);
        return JIM_OK;
    }

    if (strcmp(subcmd, "count") == 0) {
        int count = 0;
        ble_store_util_count(BLE_STORE_OBJ_TYPE_OUR_SEC, &count);
        Jim_SetResultInt(interp, count);
        return JIM_OK;
    }

    Jim_SetResultFormatted(interp,
        "unknown bonds subcommand \"%s\": should be list, clear, remove, or count", subcmd);
    return JIM_ERR;
}

/* ---------------------------------------------------------------------------
 * Subcommand: ble passkey <conn_handle> <passkey>
 * Input a passkey when the remote device requests passkey entry.
 * ---------------------------------------------------------------------------*/

static int ble_cmd_passkey_input(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    long conn_handle, passkey;
    if (Jim_GetLong(interp, argv[0], &conn_handle) != JIM_OK) return JIM_ERR;
    if (Jim_GetLong(interp, argv[1], &passkey) != JIM_OK) return JIM_ERR;

    if (passkey < 0 || passkey > 999999) {
        Jim_SetResultString(interp, "passkey must be 0-999999", -1);
        return JIM_ERR;
    }

    struct ble_sm_io pkey = {0};
    pkey.action = BLE_SM_IOACT_INPUT;
    pkey.passkey = (uint32_t)passkey;

    int rc = ble_sm_inject_io((uint16_t)conn_handle, &pkey);
    if (rc != 0) {
        Jim_SetResultFormatted(interp, "passkey input failed: %d", rc);
        return JIM_ERR;
    }

    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Subcommand: ble pair <conn_handle>
 * Initiate security/pairing on an existing connection.
 * ---------------------------------------------------------------------------*/

static int ble_cmd_pair(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    long conn_handle;
    if (Jim_GetLong(interp, argv[0], &conn_handle) != JIM_OK) return JIM_ERR;

    int rc = ble_gap_security_initiate((uint16_t)conn_handle);
    if (rc != 0) {
        Jim_SetResultFormatted(interp, "pairing initiate failed: %d", rc);
        return JIM_ERR;
    }

    Jim_SetResultString(interp, "ok", -1);
    return JIM_OK;
}

static const jim_subcmd_type ble_command_table[] = {
    {   "init",
        "?-name device_name?",
        ble_cmd_init,
        0,
        -1,
        /* Description: Initialize NimBLE stack */
    },
    {   "deinit",
        "",
        ble_cmd_deinit,
        0,
        0,
        /* Description: Deinitialize NimBLE stack */
    },
    {   "address",
        "",
        ble_cmd_address,
        0,
        0,
        /* Description: Get BLE MAC address */
    },
    {   "service",
        "add|list ...",
        ble_cmd_service,
        1,
        -1,
        /* Description: Manage GATT services */
    },
    {   "advertise",
        "start|stop ...",
        ble_cmd_advertise,
        1,
        -1,
        /* Description: Control advertising */
    },
    {   "char",
        "set|get handle ?value?",
        ble_cmd_char,
        2,
        -1,
        /* Description: Get/set characteristic values */
    },
    {   "notify",
        "handle ?-conn conn_handle?",
        ble_cmd_notify,
        1,
        -1,
        /* Description: Send notification to subscribed clients */
    },
    {   "scan",
        "start|stop|results ...",
        ble_cmd_scan,
        1,
        -1,
        /* Description: BLE scanning */
    },
    {   "connect",
        "addr ?-addr_type type? ?-timeout ms?",
        ble_cmd_connect,
        1,
        -1,
        /* Description: Connect to a BLE peripheral */
    },
    {   "disconnect",
        "conn_handle",
        ble_cmd_disconnect,
        1,
        1,
        /* Description: Disconnect a BLE connection */
    },
    {   "discover",
        "conn_handle",
        ble_cmd_discover,
        1,
        1,
        /* Description: Discover services and characteristics */
    },
    {   "read",
        "conn_handle attr_handle",
        ble_cmd_read,
        2,
        2,
        /* Description: Read a remote characteristic */
    },
    {   "write",
        "conn_handle attr_handle data",
        ble_cmd_write,
        3,
        3,
        /* Description: Write to a remote characteristic */
    },
    {   "subscribe",
        "conn_handle cccd_handle ?-notify? ?-indicate?",
        ble_cmd_subscribe,
        2,
        -1,
        /* Description: Subscribe to notifications/indications */
    },
    {   "status",
        "",
        ble_cmd_status,
        0,
        0,
        /* Description: Get BLE status */
    },
    {   "connections",
        "",
        ble_cmd_connections,
        0,
        0,
        /* Description: List active connections */
    },
    {   "on",
        "event ?-callback {proc task}? ?-remove?",
        ble_cmd_on,
        1,
        -1,
        /* Description: Set event callbacks */
    },
    {   "bonds",
        "list|clear|remove <addr>|count",
        ble_cmd_bonds,
        1,
        -1,
        /* Description: Manage bonded devices (list, remove, clear all) */
    },
    {   "passkey",
        "conn_handle passkey",
        ble_cmd_passkey_input,
        2,
        2,
        /* Description: Input passkey for pairing (when passkey entry requested) */
    },
    {   "pair",
        "conn_handle",
        ble_cmd_pair,
        1,
        1,
        /* Description: Initiate pairing/bonding with connected device */
    },
    { NULL }
};

int Jim_bleInit(Jim_Interp *interp)
{
    Jim_PackageProvideCheck(interp, "ble");
    Jim_RegisterSubCmd(interp, "ble", ble_command_table, NULL);
    return JIM_OK;
}
