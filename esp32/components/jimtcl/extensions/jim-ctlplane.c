/* Jim Tcl Control Plane Extension for ESP32
 *
 * Provides a binary-protocol control plane over UART (serial) transport,
 * using COBS framing and MessagePack encoding. Enables remote management
 * of task VMs, system info queries, and script evaluation.
 *
 * Tcl commands:
 *   ctlplane start serial <port> -tx <pin> -rx <pin> -baud <rate>
 *   ctlplane stop
 *   ctlplane status
 *   ctlplane auth <key>
 */

#include <string.h>
#include <stdlib.h>
#include "jim.h"
#include "jim-subcmd.h"
#include "jim-esp32-task.h"
#include "jim-mpack.h"
#include "mpack/mpack.h"
#include "cobs.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "jim-ctlplane";

/* ---------------------------------------------------------------------------
 * Configuration defaults
 * ---------------------------------------------------------------------------*/

#ifndef CONFIG_JIM_CTLPLANE_AUTH_KEY
#define CONFIG_JIM_CTLPLANE_AUTH_KEY ""
#endif

#ifndef CONFIG_JIM_CTLPLANE_AUTH_TIMEOUT
#define CONFIG_JIM_CTLPLANE_AUTH_TIMEOUT 300  /* seconds */
#endif

#define CTLPLANE_RX_BUF      4096
#define CTLPLANE_TX_BUF      0
#define CTLPLANE_STACK_SIZE   8192
#define CTLPLANE_PRIORITY     6
#define CTLPLANE_MAX_FRAME    4096
#define CTLPLANE_RESPONSE_BUF 4096

/* ---------------------------------------------------------------------------
 * Static state
 * ---------------------------------------------------------------------------*/

typedef struct {
    int running;
    TaskHandle_t task_handle;
    int transport;            /* 0=serial, 1=websocket */
    int uart_port;            /* for serial transport */
    int authenticated;        /* session auth state */
    int64_t last_activity_us; /* for auth timeout */
    Jim_Interp *main_interp;  /* main interpreter for eval commands */
    volatile int stop_flag;   /* signal the task to stop */
} ctlplane_state_t;

static ctlplane_state_t ctlplane_state = {0};

/* ---------------------------------------------------------------------------
 * Response helpers
 * ---------------------------------------------------------------------------*/

static void write_error(mpack_writer_t *w, const char *msg)
{
    mpack_start_map(w, 2);
    mpack_write_cstr(w, "status");
    mpack_write_cstr(w, "error");
    mpack_write_cstr(w, "message");
    mpack_write_cstr(w, msg);
    mpack_finish_map(w);
}

static void write_auth_required(mpack_writer_t *w)
{
    mpack_start_map(w, 1);
    mpack_write_cstr(w, "status");
    mpack_write_cstr(w, "auth_required");
    mpack_finish_map(w);
}

static void write_ok(mpack_writer_t *w)
{
    mpack_start_map(w, 1);
    mpack_write_cstr(w, "status");
    mpack_write_cstr(w, "ok");
    mpack_finish_map(w);
}

/* ---------------------------------------------------------------------------
 * Serial transport: send response
 * ---------------------------------------------------------------------------*/

static void send_response_serial(int uart_port, const char *data, size_t len)
{
    uint8_t *cobs_buf = malloc(COBS_MAX_ENCODED_SIZE(len));
    if (!cobs_buf) {
        ESP_LOGE(TAG, "Failed to allocate COBS buffer for response");
        return;
    }
    size_t cobs_len = cobs_encode((const uint8_t *)data, len,
                                  cobs_buf, COBS_MAX_ENCODED_SIZE(len));
    uart_write_bytes((uart_port_t)uart_port, cobs_buf, cobs_len);
    uint8_t delimiter = 0x00;
    uart_write_bytes((uart_port_t)uart_port, &delimiter, 1);
    free(cobs_buf);
}

/* ---------------------------------------------------------------------------
 * Authentication check
 * ---------------------------------------------------------------------------*/

static int ctlplane_auth_required(void)
{
    return strlen(CONFIG_JIM_CTLPLANE_AUTH_KEY) > 0;
}

static int ctlplane_check_auth(ctlplane_state_t *state)
{
    if (!ctlplane_auth_required()) {
        return 1;  /* no auth configured, always authenticated */
    }
    if (!state->authenticated) {
        return 0;
    }
    /* Check timeout */
    int64_t now = esp_timer_get_time();
    int64_t timeout_us = (int64_t)CONFIG_JIM_CTLPLANE_AUTH_TIMEOUT * 1000000LL;
    if (timeout_us > 0 && (now - state->last_activity_us) > timeout_us) {
        ESP_LOGW(TAG, "Auth session timed out");
        state->authenticated = 0;
        return 0;
    }
    state->last_activity_us = now;
    return 1;
}

/* ---------------------------------------------------------------------------
 * Task state string helper
 * ---------------------------------------------------------------------------*/

static const char *task_state_str(task_state_t s)
{
    switch (s) {
        case TASK_STATE_STOPPED:  return "stopped";
        case TASK_STATE_STARTING: return "starting";
        case TASK_STATE_RUNNING:  return "running";
        default:                  return "unknown";
    }
}

static const char *cb_state_str(cb_state_t s)
{
    switch (s) {
        case CB_CLOSED:    return "closed";
        case CB_OPEN:      return "open";
        case CB_HALF_OPEN: return "half_open";
        default:           return "unknown";
    }
}

/* ---------------------------------------------------------------------------
 * Command handlers
 * ---------------------------------------------------------------------------*/

static void handle_vm_list(ctlplane_state_t *state, mpack_node_t root,
                           mpack_writer_t *writer)
{
    (void)root;

    /* Count active slots */
    int count = 0;
    if (xSemaphoreTake(task_slots_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        write_error(writer, "failed to acquire task slots mutex");
        return;
    }
    for (int i = 0; i < TASK_MAX_SLOTS; i++) {
        if (task_slots[i].in_use) count++;
    }

    mpack_start_map(writer, 2);
    mpack_write_cstr(writer, "status");
    mpack_write_cstr(writer, "ok");
    mpack_write_cstr(writer, "vms");

    mpack_start_array(writer, (uint32_t)count);
    for (int i = 0; i < TASK_MAX_SLOTS; i++) {
        if (!task_slots[i].in_use) continue;
        mpack_start_map(writer, 3);
        mpack_write_cstr(writer, "id");
        mpack_write_int(writer, i);
        mpack_write_cstr(writer, "name");
        mpack_write_cstr(writer, task_slots[i].name);
        mpack_write_cstr(writer, "state");
        mpack_write_cstr(writer, task_state_str(task_slots[i].state));
        mpack_finish_map(writer);
    }
    mpack_finish_array(writer);

    xSemaphoreGive(task_slots_mutex);
    mpack_finish_map(writer);
}

static void handle_vm_info(ctlplane_state_t *state, mpack_node_t root,
                           mpack_writer_t *writer)
{
    mpack_node_t target_node = mpack_node_map_cstr(root, "target");
    if (mpack_node_is_missing(target_node)) {
        write_error(writer, "missing 'target' field");
        return;
    }
    const char *target = mpack_node_str(target_node);
    size_t target_len = mpack_node_strlen(target_node);
    char name_buf[16];
    size_t copy_len = target_len < sizeof(name_buf) - 1 ? target_len : sizeof(name_buf) - 1;
    memcpy(name_buf, target, copy_len);
    name_buf[copy_len] = '\0';

    if (xSemaphoreTake(task_slots_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        write_error(writer, "failed to acquire task slots mutex");
        return;
    }

    int idx = task_find_slot_by_name(name_buf);
    if (idx < 0) {
        xSemaphoreGive(task_slots_mutex);
        write_error(writer, "vm not found");
        return;
    }

    task_slot_t *slot = &task_slots[idx];

    mpack_start_map(writer, 10);
    mpack_write_cstr(writer, "status");
    mpack_write_cstr(writer, "ok");
    mpack_write_cstr(writer, "id");
    mpack_write_int(writer, idx);
    mpack_write_cstr(writer, "name");
    mpack_write_cstr(writer, slot->name);
    mpack_write_cstr(writer, "state");
    mpack_write_cstr(writer, task_state_str(slot->state));
    mpack_write_cstr(writer, "stacksize");
    mpack_write_u32(writer, slot->stacksize);
    mpack_write_cstr(writer, "priority");
    mpack_write_u32(writer, (uint32_t)slot->priority);
    mpack_write_cstr(writer, "auto_restart");
    mpack_write_bool(writer, slot->auto_restart);
    mpack_write_cstr(writer, "restart_count");
    mpack_write_int(writer, slot->restart_count);
    mpack_write_cstr(writer, "cb_state");
    mpack_write_cstr(writer, cb_state_str(slot->cb_state));
    mpack_write_cstr(writer, "last_activity_us");
    mpack_write_i64(writer, slot->last_activity_us);

    xSemaphoreGive(task_slots_mutex);
    mpack_finish_map(writer);
}

static void handle_vm_eval(ctlplane_state_t *state, mpack_node_t root,
                           mpack_writer_t *writer)
{
    mpack_node_t target_node = mpack_node_map_cstr(root, "target");
    mpack_node_t script_node = mpack_node_map_cstr(root, "script");
    if (mpack_node_is_missing(target_node) || mpack_node_is_missing(script_node)) {
        write_error(writer, "missing 'target' or 'script' field");
        return;
    }

    const char *target = mpack_node_str(target_node);
    size_t target_len = mpack_node_strlen(target_node);
    char name_buf[16];
    size_t copy_len = target_len < sizeof(name_buf) - 1 ? target_len : sizeof(name_buf) - 1;
    memcpy(name_buf, target, copy_len);
    name_buf[copy_len] = '\0';

    const char *script = mpack_node_str(script_node);
    size_t script_len = mpack_node_strlen(script_node);
    char *script_buf = malloc(script_len + 1);
    if (!script_buf) {
        write_error(writer, "out of memory");
        return;
    }
    memcpy(script_buf, script, script_len);
    script_buf[script_len] = '\0';

    if (xSemaphoreTake(task_slots_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        free(script_buf);
        write_error(writer, "failed to acquire task slots mutex");
        return;
    }

    int idx = task_find_slot_by_name(name_buf);
    if (idx < 0) {
        xSemaphoreGive(task_slots_mutex);
        free(script_buf);
        write_error(writer, "vm not found");
        return;
    }

    task_slot_t *slot = &task_slots[idx];
    if (slot->state != TASK_STATE_RUNNING) {
        xSemaphoreGive(task_slots_mutex);
        free(script_buf);
        write_error(writer, "vm not running");
        return;
    }

    /* Create a temporary reply queue */
    QueueHandle_t reply_queue = xQueueCreate(1, sizeof(task_reply_t));
    if (!reply_queue) {
        xSemaphoreGive(task_slots_mutex);
        free(script_buf);
        write_error(writer, "failed to create reply queue");
        return;
    }

    /* Send eval message */
    task_msg_t msg = {
        .type = TASK_MSG_EVAL,
        .script = script_buf,  /* ownership transferred to receiver */
        .reply_queue = reply_queue,
    };

    if (xQueueSend(slot->msg_queue, &msg, pdMS_TO_TICKS(1000)) != pdTRUE) {
        xSemaphoreGive(task_slots_mutex);
        vQueueDelete(reply_queue);
        free(script_buf);
        write_error(writer, "message queue full");
        return;
    }

    xSemaphoreGive(task_slots_mutex);

    /* Wait for reply with 30s timeout */
    task_reply_t reply;
    if (xQueueReceive(reply_queue, &reply, pdMS_TO_TICKS(30000)) != pdTRUE) {
        vQueueDelete(reply_queue);
        write_error(writer, "eval timeout");
        return;
    }

    vQueueDelete(reply_queue);

    mpack_start_map(writer, 3);
    mpack_write_cstr(writer, "status");
    mpack_write_cstr(writer, "ok");
    mpack_write_cstr(writer, "retcode");
    mpack_write_int(writer, reply.retcode);
    mpack_write_cstr(writer, "result");
    mpack_write_cstr(writer, reply.result ? reply.result : "");
    mpack_finish_map(writer);

    if (reply.result) free(reply.result);
}

static void handle_vm_send(ctlplane_state_t *state, mpack_node_t root,
                           mpack_writer_t *writer)
{
    mpack_node_t target_node = mpack_node_map_cstr(root, "target");
    mpack_node_t script_node = mpack_node_map_cstr(root, "script");
    if (mpack_node_is_missing(target_node) || mpack_node_is_missing(script_node)) {
        write_error(writer, "missing 'target' or 'script' field");
        return;
    }

    const char *target = mpack_node_str(target_node);
    size_t target_len = mpack_node_strlen(target_node);
    char name_buf[16];
    size_t copy_len = target_len < sizeof(name_buf) - 1 ? target_len : sizeof(name_buf) - 1;
    memcpy(name_buf, target, copy_len);
    name_buf[copy_len] = '\0';

    const char *script = mpack_node_str(script_node);
    size_t script_len = mpack_node_strlen(script_node);
    char *script_buf = malloc(script_len + 1);
    if (!script_buf) {
        write_error(writer, "out of memory");
        return;
    }
    memcpy(script_buf, script, script_len);
    script_buf[script_len] = '\0';

    int ret = task_send_to_name(name_buf, script_buf);
    free(script_buf);

    if (ret != 0) {
        write_error(writer, "send failed: target not found or queue full");
        return;
    }

    write_ok(writer);
}

static void handle_vm_create(ctlplane_state_t *state, mpack_node_t root,
                             mpack_writer_t *writer)
{
    /* VM creation requires evaluating a task create command on the main interp */
    mpack_node_t name_node = mpack_node_map_cstr(root, "name");
    mpack_node_t script_node = mpack_node_map_cstr(root, "script");
    if (mpack_node_is_missing(name_node) || mpack_node_is_missing(script_node)) {
        write_error(writer, "missing 'name' or 'script' field");
        return;
    }

    const char *name = mpack_node_str(name_node);
    size_t name_len = mpack_node_strlen(name_node);
    const char *script = mpack_node_str(script_node);
    size_t script_len = mpack_node_strlen(script_node);

    /* Build a task create command */
    size_t cmd_len = 64 + name_len + script_len;
    char *cmd = malloc(cmd_len);
    if (!cmd) {
        write_error(writer, "out of memory");
        return;
    }

    char name_buf[16];
    size_t copy_len = name_len < sizeof(name_buf) - 1 ? name_len : sizeof(name_buf) - 1;
    memcpy(name_buf, name, copy_len);
    name_buf[copy_len] = '\0';

    char *script_buf = malloc(script_len + 1);
    if (!script_buf) {
        free(cmd);
        write_error(writer, "out of memory");
        return;
    }
    memcpy(script_buf, script, script_len);
    script_buf[script_len] = '\0';

    snprintf(cmd, cmd_len, "task create %s {%s}", name_buf, script_buf);
    free(script_buf);

    int retcode = Jim_Eval(state->main_interp, cmd);
    const char *result = Jim_String(Jim_GetResult(state->main_interp));

    mpack_start_map(writer, 3);
    mpack_write_cstr(writer, "status");
    mpack_write_cstr(writer, retcode == JIM_OK ? "ok" : "error");
    mpack_write_cstr(writer, "retcode");
    mpack_write_int(writer, retcode);
    mpack_write_cstr(writer, "result");
    mpack_write_cstr(writer, result);
    mpack_finish_map(writer);

    free(cmd);
}

static void handle_vm_delete(ctlplane_state_t *state, mpack_node_t root,
                             mpack_writer_t *writer)
{
    mpack_node_t target_node = mpack_node_map_cstr(root, "target");
    if (mpack_node_is_missing(target_node)) {
        write_error(writer, "missing 'target' field");
        return;
    }

    const char *target = mpack_node_str(target_node);
    size_t target_len = mpack_node_strlen(target_node);
    char name_buf[16];
    size_t copy_len = target_len < sizeof(name_buf) - 1 ? target_len : sizeof(name_buf) - 1;
    memcpy(name_buf, target, copy_len);
    name_buf[copy_len] = '\0';

    /* Use task kill command on main interp */
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "task kill %s", name_buf);
    int retcode = Jim_Eval(state->main_interp, cmd);
    const char *result = Jim_String(Jim_GetResult(state->main_interp));

    mpack_start_map(writer, 3);
    mpack_write_cstr(writer, "status");
    mpack_write_cstr(writer, retcode == JIM_OK ? "ok" : "error");
    mpack_write_cstr(writer, "retcode");
    mpack_write_int(writer, retcode);
    mpack_write_cstr(writer, "result");
    mpack_write_cstr(writer, result);
    mpack_finish_map(writer);
}

static void handle_vm_restart(ctlplane_state_t *state, mpack_node_t root,
                              mpack_writer_t *writer)
{
    mpack_node_t target_node = mpack_node_map_cstr(root, "target");
    if (mpack_node_is_missing(target_node)) {
        write_error(writer, "missing 'target' field");
        return;
    }

    const char *target = mpack_node_str(target_node);
    size_t target_len = mpack_node_strlen(target_node);
    char name_buf[16];
    size_t copy_len = target_len < sizeof(name_buf) - 1 ? target_len : sizeof(name_buf) - 1;
    memcpy(name_buf, target, copy_len);
    name_buf[copy_len] = '\0';

    if (xSemaphoreTake(task_slots_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        write_error(writer, "failed to acquire task slots mutex");
        return;
    }

    int idx = task_find_slot_by_name(name_buf);
    if (idx < 0) {
        xSemaphoreGive(task_slots_mutex);
        write_error(writer, "vm not found");
        return;
    }

    int ret = task_restart(idx);
    xSemaphoreGive(task_slots_mutex);

    if (ret == 0) {
        write_ok(writer);
    } else if (ret == -2) {
        write_error(writer, "circuit breaker open");
    } else {
        write_error(writer, "restart failed");
    }
}

static void handle_sys_info(ctlplane_state_t *state, mpack_node_t root,
                            mpack_writer_t *writer)
{
    (void)root;

    esp_chip_info_t chip;
    esp_chip_info(&chip);

    mpack_start_map(writer, 7);
    mpack_write_cstr(writer, "status");
    mpack_write_cstr(writer, "ok");
    mpack_write_cstr(writer, "cores");
    mpack_write_int(writer, chip.cores);
    mpack_write_cstr(writer, "revision");
    mpack_write_int(writer, chip.revision);
    mpack_write_cstr(writer, "heap_free");
    mpack_write_u32(writer, (uint32_t)esp_get_free_heap_size());
    mpack_write_cstr(writer, "heap_min");
    mpack_write_u32(writer, (uint32_t)esp_get_minimum_free_heap_size());
    mpack_write_cstr(writer, "uptime_us");
    mpack_write_i64(writer, esp_timer_get_time());
    mpack_write_cstr(writer, "model");
    mpack_write_int(writer, (int)chip.model);
    mpack_finish_map(writer);
}

static void handle_sys_heap(ctlplane_state_t *state, mpack_node_t root,
                            mpack_writer_t *writer)
{
    (void)root;

    mpack_start_map(writer, 3);
    mpack_write_cstr(writer, "status");
    mpack_write_cstr(writer, "ok");
    mpack_write_cstr(writer, "free");
    mpack_write_u32(writer, (uint32_t)esp_get_free_heap_size());
    mpack_write_cstr(writer, "minimum");
    mpack_write_u32(writer, (uint32_t)esp_get_minimum_free_heap_size());
    mpack_finish_map(writer);
}

static void handle_sys_wifi(ctlplane_state_t *state, mpack_node_t root,
                            mpack_writer_t *writer)
{
    (void)root;

    wifi_ap_record_t ap_info;
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
    if (err != ESP_OK) {
        mpack_start_map(writer, 2);
        mpack_write_cstr(writer, "status");
        mpack_write_cstr(writer, "ok");
        mpack_write_cstr(writer, "connected");
        mpack_write_bool(writer, false);
        mpack_finish_map(writer);
        return;
    }

    /* Get IP info */
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info = {0};
    if (netif) {
        esp_netif_get_ip_info(netif, &ip_info);
    }

    char ip_str[16];
    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));

    mpack_start_map(writer, 5);
    mpack_write_cstr(writer, "status");
    mpack_write_cstr(writer, "ok");
    mpack_write_cstr(writer, "connected");
    mpack_write_bool(writer, true);
    mpack_write_cstr(writer, "ssid");
    mpack_write_cstr(writer, (const char *)ap_info.ssid);
    mpack_write_cstr(writer, "rssi");
    mpack_write_int(writer, ap_info.rssi);
    mpack_write_cstr(writer, "ip");
    mpack_write_cstr(writer, ip_str);
    mpack_finish_map(writer);
}

static void handle_sys_uptime(ctlplane_state_t *state, mpack_node_t root,
                              mpack_writer_t *writer)
{
    (void)root;

    mpack_start_map(writer, 2);
    mpack_write_cstr(writer, "status");
    mpack_write_cstr(writer, "ok");
    mpack_write_cstr(writer, "uptime_us");
    mpack_write_i64(writer, esp_timer_get_time());
    mpack_finish_map(writer);
}

static void handle_eval(ctlplane_state_t *state, mpack_node_t root,
                        mpack_writer_t *writer)
{
    mpack_node_t script_node = mpack_node_map_cstr(root, "script");
    if (mpack_node_is_missing(script_node)) {
        write_error(writer, "missing 'script' field");
        return;
    }

    const char *script = mpack_node_str(script_node);
    size_t script_len = mpack_node_strlen(script_node);
    char *script_buf = malloc(script_len + 1);
    if (!script_buf) {
        write_error(writer, "out of memory");
        return;
    }
    memcpy(script_buf, script, script_len);
    script_buf[script_len] = '\0';

    int retcode = Jim_Eval(state->main_interp, script_buf);
    const char *result = Jim_String(Jim_GetResult(state->main_interp));
    free(script_buf);

    mpack_start_map(writer, 3);
    mpack_write_cstr(writer, "status");
    mpack_write_cstr(writer, retcode == JIM_OK ? "ok" : "error");
    mpack_write_cstr(writer, "retcode");
    mpack_write_int(writer, retcode);
    mpack_write_cstr(writer, "result");
    mpack_write_cstr(writer, result);
    mpack_finish_map(writer);
}

static void handle_auth(ctlplane_state_t *state, mpack_node_t root,
                        mpack_writer_t *writer)
{
    if (!ctlplane_auth_required()) {
        /* Auth not configured, always ok */
        mpack_start_map(writer, 2);
        mpack_write_cstr(writer, "status");
        mpack_write_cstr(writer, "ok");
        mpack_write_cstr(writer, "message");
        mpack_write_cstr(writer, "auth not required");
        mpack_finish_map(writer);
        return;
    }

    mpack_node_t key_node = mpack_node_map_cstr(root, "key");
    if (mpack_node_is_missing(key_node)) {
        write_error(writer, "missing 'key' field");
        return;
    }

    const char *key = mpack_node_str(key_node);
    size_t key_len = mpack_node_strlen(key_node);

    size_t expected_len = strlen(CONFIG_JIM_CTLPLANE_AUTH_KEY);
    if (key_len == expected_len &&
        memcmp(key, CONFIG_JIM_CTLPLANE_AUTH_KEY, expected_len) == 0) {
        state->authenticated = 1;
        state->last_activity_us = esp_timer_get_time();
        ESP_LOGI(TAG, "Control plane authenticated");

        mpack_start_map(writer, 1);
        mpack_write_cstr(writer, "status");
        mpack_write_cstr(writer, "ok");
        mpack_finish_map(writer);
    } else {
        ESP_LOGW(TAG, "Control plane auth failed: bad key");
        write_error(writer, "invalid key");
    }
}

static void handle_vars_load(ctlplane_state_t *state, mpack_node_t root,
                             mpack_writer_t *writer)
{
    mpack_node_t vars_node = mpack_node_map_cstr(root, "vars");
    if (mpack_node_is_missing(vars_node)) {
        write_error(writer, "missing 'vars' field");
        return;
    }

    /* Determine target interpreter */
    Jim_Interp *target_interp = state->main_interp;
    mpack_node_t target_node = mpack_node_map_cstr(root, "target");
    if (!mpack_node_is_missing(target_node)) {
        const char *target = mpack_node_str(target_node);
        size_t target_len = mpack_node_strlen(target_node);
        char name_buf[16];
        size_t copy_len = target_len < sizeof(name_buf) - 1 ? target_len : sizeof(name_buf) - 1;
        memcpy(name_buf, target, copy_len);
        name_buf[copy_len] = '\0';

        if (xSemaphoreTake(task_slots_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
            write_error(writer, "failed to acquire task slots mutex");
            return;
        }
        int idx = task_find_slot_by_name(name_buf);
        if (idx < 0) {
            xSemaphoreGive(task_slots_mutex);
            write_error(writer, "target vm not found");
            return;
        }
        target_interp = task_slots[idx].interp;
        xSemaphoreGive(task_slots_mutex);

        if (!target_interp) {
            write_error(writer, "target vm has no interpreter");
            return;
        }
    }

    /* Iterate the vars map and set each variable */
    size_t count = mpack_node_map_count(vars_node);
    int errors = 0;
    for (size_t i = 0; i < count; i++) {
        mpack_node_t key_node = mpack_node_map_key_at(vars_node, i);
        mpack_node_t val_node = mpack_node_map_value_at(vars_node, i);

        const char *key = mpack_node_str(key_node);
        size_t key_len = mpack_node_strlen(key_node);
        const char *val = mpack_node_str(val_node);
        size_t val_len = mpack_node_strlen(val_node);

        char *key_buf = malloc(key_len + 1);
        char *val_buf = malloc(val_len + 1);
        if (!key_buf || !val_buf) {
            free(key_buf);
            free(val_buf);
            errors++;
            continue;
        }
        memcpy(key_buf, key, key_len);
        key_buf[key_len] = '\0';
        memcpy(val_buf, val, val_len);
        val_buf[val_len] = '\0';

        Jim_SetVariableStrWithStr(target_interp, key_buf, val_buf);
        free(key_buf);
        free(val_buf);
    }

    if (errors > 0) {
        write_error(writer, "some variables failed to set");
    } else {
        mpack_start_map(writer, 2);
        mpack_write_cstr(writer, "status");
        mpack_write_cstr(writer, "ok");
        mpack_write_cstr(writer, "count");
        mpack_write_uint(writer, (uint32_t)count);
        mpack_finish_map(writer);
    }
}

static void handle_vars_get(ctlplane_state_t *state, mpack_node_t root,
                            mpack_writer_t *writer)
{
    mpack_node_t names_node = mpack_node_map_cstr(root, "names");
    if (mpack_node_is_missing(names_node)) {
        write_error(writer, "missing 'names' field");
        return;
    }

    /* Determine target interpreter */
    Jim_Interp *target_interp = state->main_interp;
    mpack_node_t target_node = mpack_node_map_cstr(root, "target");
    if (!mpack_node_is_missing(target_node)) {
        const char *target = mpack_node_str(target_node);
        size_t target_len = mpack_node_strlen(target_node);
        char name_buf[16];
        size_t copy_len = target_len < sizeof(name_buf) - 1 ? target_len : sizeof(name_buf) - 1;
        memcpy(name_buf, target, copy_len);
        name_buf[copy_len] = '\0';

        if (xSemaphoreTake(task_slots_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
            write_error(writer, "failed to acquire task slots mutex");
            return;
        }
        int idx = task_find_slot_by_name(name_buf);
        if (idx < 0) {
            xSemaphoreGive(task_slots_mutex);
            write_error(writer, "target vm not found");
            return;
        }
        target_interp = task_slots[idx].interp;
        xSemaphoreGive(task_slots_mutex);

        if (!target_interp) {
            write_error(writer, "target vm has no interpreter");
            return;
        }
    }

    size_t count = mpack_node_array_length(names_node);

    mpack_start_map(writer, 2);
    mpack_write_cstr(writer, "status");
    mpack_write_cstr(writer, "ok");
    mpack_write_cstr(writer, "vars");

    mpack_start_map(writer, (uint32_t)count);
    for (size_t i = 0; i < count; i++) {
        mpack_node_t name_node = mpack_node_array_at(names_node, i);
        const char *name = mpack_node_str(name_node);
        size_t name_len = mpack_node_strlen(name_node);

        char *name_buf = malloc(name_len + 1);
        if (!name_buf) continue;
        memcpy(name_buf, name, name_len);
        name_buf[name_len] = '\0';

        Jim_Obj *val = Jim_GetVariableStr(target_interp, name_buf, JIM_NONE);
        if (val) {
            mpack_write_cstr(writer, name_buf);
            mpack_write_cstr(writer, Jim_String(val));
        } else {
            mpack_write_cstr(writer, name_buf);
            mpack_write_nil(writer);
        }
        free(name_buf);
    }
    mpack_finish_map(writer);

    mpack_finish_map(writer);
}

/* ---------------------------------------------------------------------------
 * Command dispatch table
 * ---------------------------------------------------------------------------*/

typedef void (*ctlplane_handler_t)(ctlplane_state_t *state, mpack_node_t root,
                                   mpack_writer_t *writer);

typedef struct {
    const char *cmd;
    ctlplane_handler_t handler;
    int requires_auth;  /* 1 = requires auth, 0 = always allowed */
} ctlplane_cmd_entry_t;

static const ctlplane_cmd_entry_t ctlplane_commands[] = {
    { "auth",        handle_auth,        0 },
    { "vm.list",     handle_vm_list,     1 },
    { "vm.info",     handle_vm_info,     1 },
    { "vm.eval",     handle_vm_eval,     1 },
    { "vm.send",     handle_vm_send,     1 },
    { "vm.create",   handle_vm_create,   1 },
    { "vm.delete",   handle_vm_delete,   1 },
    { "vm.restart",  handle_vm_restart,  1 },
    { "sys.info",    handle_sys_info,    1 },
    { "sys.heap",    handle_sys_heap,    1 },
    { "sys.wifi",    handle_sys_wifi,    1 },
    { "sys.uptime",  handle_sys_uptime,  1 },
    { "eval",        handle_eval,        1 },
    { "vars.load",   handle_vars_load,   1 },
    { "vars.get",    handle_vars_get,    1 },
    { NULL,          NULL,               0 },
};

/* ---------------------------------------------------------------------------
 * Frame processing
 * ---------------------------------------------------------------------------*/

static void ctlplane_process_frame(ctlplane_state_t *state,
                                   const uint8_t *frame, size_t frame_len)
{
    /* Decode COBS */
    uint8_t *decoded = malloc(frame_len);
    if (!decoded) {
        ESP_LOGE(TAG, "Failed to allocate decode buffer");
        return;
    }
    size_t decoded_len = cobs_decode(frame, frame_len, decoded, frame_len);
    if (decoded_len == 0) {
        ESP_LOGW(TAG, "COBS decode failed");
        free(decoded);
        return;
    }

    /* Parse mpack */
    mpack_tree_t tree;
    mpack_tree_init_data(&tree, (const char *)decoded, decoded_len);
    mpack_tree_parse(&tree);

    if (mpack_tree_error(&tree) != mpack_ok) {
        ESP_LOGW(TAG, "mpack parse error: %s",
                 mpack_error_to_string(mpack_tree_error(&tree)));
        mpack_tree_destroy(&tree);
        free(decoded);
        return;
    }

    mpack_node_t root = mpack_tree_root(&tree);

    /* Extract command string */
    mpack_node_t cmd_node = mpack_node_map_cstr(root, "cmd");
    if (mpack_node_is_missing(cmd_node)) {
        ESP_LOGW(TAG, "Frame missing 'cmd' field");
        /* Send error response */
        char resp_buf[CTLPLANE_RESPONSE_BUF];
        mpack_writer_t writer;
        mpack_writer_init(&writer, resp_buf, sizeof(resp_buf));
        write_error(&writer, "missing 'cmd' field");
        size_t resp_len = mpack_writer_buffer_used(&writer);
        if (mpack_writer_destroy(&writer) == mpack_ok) {
            send_response_serial(state->uart_port, resp_buf, resp_len);
        }
        mpack_tree_destroy(&tree);
        free(decoded);
        return;
    }

    const char *cmd = mpack_node_str(cmd_node);
    size_t cmd_len = mpack_node_strlen(cmd_node);

    /* Find handler */
    const ctlplane_cmd_entry_t *entry = NULL;
    for (int i = 0; ctlplane_commands[i].cmd != NULL; i++) {
        if (strlen(ctlplane_commands[i].cmd) == cmd_len &&
            memcmp(ctlplane_commands[i].cmd, cmd, cmd_len) == 0) {
            entry = &ctlplane_commands[i];
            break;
        }
    }

    /* Prepare response writer */
    char resp_buf[CTLPLANE_RESPONSE_BUF];
    mpack_writer_t writer;
    mpack_writer_init(&writer, resp_buf, sizeof(resp_buf));

    if (!entry) {
        ESP_LOGW(TAG, "Unknown control plane command");
        write_error(&writer, "unknown command");
    } else if (entry->requires_auth && !ctlplane_check_auth(state)) {
        write_auth_required(&writer);
    } else {
        entry->handler(state, root, &writer);
    }

    size_t resp_len = mpack_writer_buffer_used(&writer);
    mpack_error_t werr = mpack_writer_destroy(&writer);
    if (werr == mpack_ok) {
        send_response_serial(state->uart_port, resp_buf, resp_len);
    } else {
        ESP_LOGE(TAG, "mpack writer error: %s", mpack_error_to_string(werr));
    }

    mpack_tree_destroy(&tree);
    free(decoded);
}

/* ---------------------------------------------------------------------------
 * Background task: serial transport
 * ---------------------------------------------------------------------------*/

static void ctlplane_task(void *param)
{
    ctlplane_state_t *state = (ctlplane_state_t *)param;

    uint8_t *frame_buf = malloc(CTLPLANE_MAX_FRAME);
    if (!frame_buf) {
        ESP_LOGE(TAG, "Failed to allocate frame buffer");
        state->running = 0;
        vTaskDelete(NULL);
        return;
    }

    size_t frame_pos = 0;
    uint8_t rx_byte;

    ESP_LOGI(TAG, "Control plane task started on UART%d", state->uart_port);

    while (!state->stop_flag) {
        /* Read one byte at a time, with timeout for stop check */
        int len = uart_read_bytes((uart_port_t)state->uart_port,
                                  &rx_byte, 1, pdMS_TO_TICKS(100));
        if (len <= 0) continue;

        if (rx_byte == 0x00) {
            /* Frame delimiter — process accumulated frame */
            if (frame_pos > 0) {
                ctlplane_process_frame(state, frame_buf, frame_pos);
                frame_pos = 0;
            }
        } else {
            /* Accumulate byte into frame */
            if (frame_pos < CTLPLANE_MAX_FRAME) {
                frame_buf[frame_pos++] = rx_byte;
            } else {
                /* Frame overflow, discard */
                ESP_LOGW(TAG, "Frame overflow, discarding %zu bytes", frame_pos);
                frame_pos = 0;
            }
        }
    }

    free(frame_buf);
    ESP_LOGI(TAG, "Control plane task stopped");
    state->running = 0;
    state->task_handle = NULL;
    vTaskDelete(NULL);
}

/* ---------------------------------------------------------------------------
 * Tcl subcommand: ctlplane start serial <port> -tx <pin> -rx <pin> -baud <rate>
 * ---------------------------------------------------------------------------*/

static int ctlplane_cmd_start(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (ctlplane_state.running) {
        Jim_SetResultString(interp, "control plane already running", -1);
        return JIM_ERR;
    }

    if (argc < 1) {
        Jim_SetResultString(interp,
            "wrong # args: should be \"ctlplane start serial port -tx pin -rx pin -baud rate\"",
            -1);
        return JIM_ERR;
    }

    const char *transport = Jim_String(argv[0]);
    if (strcmp(transport, "serial") != 0) {
        Jim_SetResultFormatted(interp,
            "unsupported transport \"%s\": should be serial", transport);
        return JIM_ERR;
    }

    if (argc < 2) {
        Jim_SetResultString(interp,
            "wrong # args: should be \"ctlplane start serial port -tx pin -rx pin -baud rate\"",
            -1);
        return JIM_ERR;
    }

    long port;
    if (Jim_GetLong(interp, argv[1], &port) != JIM_OK) return JIM_ERR;
    if (port < 0 || port > 2) {
        Jim_SetResultFormatted(interp, "invalid UART port: %ld (must be 0-2)", port);
        return JIM_ERR;
    }

    long tx_pin = -1, rx_pin = -1, baud = 115200;

    for (int i = 2; i < argc; i++) {
        const char *opt = Jim_String(argv[i]);
        if (strcmp(opt, "-tx") == 0 && i + 1 < argc) {
            if (Jim_GetLong(interp, argv[++i], &tx_pin) != JIM_OK) return JIM_ERR;
        } else if (strcmp(opt, "-rx") == 0 && i + 1 < argc) {
            if (Jim_GetLong(interp, argv[++i], &rx_pin) != JIM_OK) return JIM_ERR;
        } else if (strcmp(opt, "-baud") == 0 && i + 1 < argc) {
            if (Jim_GetLong(interp, argv[++i], &baud) != JIM_OK) return JIM_ERR;
        } else {
            Jim_SetResultFormatted(interp, "unknown option \"%s\"", opt);
            return JIM_ERR;
        }
    }

    if (tx_pin < 0 || rx_pin < 0) {
        Jim_SetResultString(interp, "must specify both -tx and -rx pins", -1);
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

    err = uart_driver_install((uart_port_t)port, CTLPLANE_RX_BUF, CTLPLANE_TX_BUF,
                              0, NULL, 0);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "uart_driver_install failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    /* Initialize state */
    ctlplane_state.transport = 0;  /* serial */
    ctlplane_state.uart_port = (int)port;
    ctlplane_state.authenticated = 0;
    ctlplane_state.last_activity_us = 0;
    ctlplane_state.main_interp = interp;
    ctlplane_state.stop_flag = 0;
    ctlplane_state.running = 1;

    /* Create background task */
    BaseType_t ret = xTaskCreate(ctlplane_task, "ctlplane", CTLPLANE_STACK_SIZE,
                                 &ctlplane_state, CTLPLANE_PRIORITY,
                                 &ctlplane_state.task_handle);
    if (ret != pdPASS) {
        uart_driver_delete((uart_port_t)port);
        ctlplane_state.running = 0;
        Jim_SetResultString(interp, "failed to create control plane task", -1);
        return JIM_ERR;
    }

    ESP_LOGI(TAG, "Control plane started: serial UART%ld TX=%ld RX=%ld baud=%ld",
             port, tx_pin, rx_pin, baud);
    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Tcl subcommand: ctlplane stop
 * ---------------------------------------------------------------------------*/

static int ctlplane_cmd_stop(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    (void)argc;
    (void)argv;

    if (!ctlplane_state.running) {
        Jim_SetResultString(interp, "control plane not running", -1);
        return JIM_ERR;
    }

    ctlplane_state.stop_flag = 1;

    /* Wait for task to exit */
    int wait = 0;
    while (ctlplane_state.task_handle && wait < 40) {
        vTaskDelay(pdMS_TO_TICKS(50));
        wait++;
    }

    if (ctlplane_state.task_handle) {
        ESP_LOGW(TAG, "Control plane task did not stop cleanly, forcing");
        vTaskDelete(ctlplane_state.task_handle);
        ctlplane_state.task_handle = NULL;
    }

    /* Clean up UART */
    uart_driver_delete((uart_port_t)ctlplane_state.uart_port);

    ctlplane_state.running = 0;
    ctlplane_state.authenticated = 0;
    ctlplane_state.stop_flag = 0;

    ESP_LOGI(TAG, "Control plane stopped");
    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Tcl subcommand: ctlplane status
 * ---------------------------------------------------------------------------*/

static int ctlplane_cmd_status(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    (void)argc;
    (void)argv;

    Jim_Obj *dict = Jim_NewListObj(interp, NULL, 0);

    Jim_ListAppendElement(interp, dict, Jim_NewStringObj(interp, "running", -1));
    Jim_ListAppendElement(interp, dict, Jim_NewIntObj(interp, ctlplane_state.running));

    Jim_ListAppendElement(interp, dict, Jim_NewStringObj(interp, "transport", -1));
    Jim_ListAppendElement(interp, dict,
        Jim_NewStringObj(interp,
            ctlplane_state.running ?
                (ctlplane_state.transport == 0 ? "serial" : "websocket") : "none",
            -1));

    Jim_ListAppendElement(interp, dict, Jim_NewStringObj(interp, "authenticated", -1));
    Jim_ListAppendElement(interp, dict, Jim_NewIntObj(interp, ctlplane_state.authenticated));

    Jim_ListAppendElement(interp, dict, Jim_NewStringObj(interp, "auth_required", -1));
    Jim_ListAppendElement(interp, dict, Jim_NewIntObj(interp, ctlplane_auth_required()));

    if (ctlplane_state.running && ctlplane_state.transport == 0) {
        Jim_ListAppendElement(interp, dict, Jim_NewStringObj(interp, "uart_port", -1));
        Jim_ListAppendElement(interp, dict,
            Jim_NewIntObj(interp, ctlplane_state.uart_port));
    }

    Jim_SetResult(interp, dict);
    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Tcl subcommand: ctlplane auth <key>
 * ---------------------------------------------------------------------------*/

static int ctlplane_cmd_auth(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc < 1) {
        Jim_SetResultString(interp,
            "wrong # args: should be \"ctlplane auth key\"", -1);
        return JIM_ERR;
    }

    if (!ctlplane_auth_required()) {
        Jim_SetResultString(interp, "auth not required", -1);
        return JIM_OK;
    }

    const char *key = Jim_String(argv[0]);
    if (strcmp(key, CONFIG_JIM_CTLPLANE_AUTH_KEY) == 0) {
        ctlplane_state.authenticated = 1;
        ctlplane_state.last_activity_us = esp_timer_get_time();
        Jim_SetResultString(interp, "ok", -1);
    } else {
        Jim_SetResultString(interp, "invalid key", -1);
        return JIM_ERR;
    }

    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Subcommand table and init
 * ---------------------------------------------------------------------------*/

static const jim_subcmd_type ctlplane_command_table[] = {
    {   "start",
        "serial port -tx pin -rx pin -baud rate",
        ctlplane_cmd_start,
        1,
        -1,
        /* Description: Start the control plane on a transport */
    },
    {   "stop",
        "",
        ctlplane_cmd_stop,
        0,
        0,
        /* Description: Stop the control plane */
    },
    {   "status",
        "",
        ctlplane_cmd_status,
        0,
        0,
        /* Description: Return control plane status as a dict */
    },
    {   "auth",
        "key",
        ctlplane_cmd_auth,
        1,
        1,
        /* Description: Programmatic authentication */
    },
    { NULL }
};

int Jim_ctlplaneInit(Jim_Interp *interp)
{
    Jim_PackageProvideCheck(interp, "ctlplane");
    Jim_RegisterSubCmd(interp, "ctlplane", ctlplane_command_table, NULL);
    return JIM_OK;
}
