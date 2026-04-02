/* Jim Tcl ESP32 Task Extension
 *
 * Enables running multiple Tcl interpreters ("VMs") on separate FreeRTOS tasks.
 * Each task gets its own Jim_Interp with full ESP32 extensions, and communicates
 * with the parent via message queues.
 *
 * Commands:
 *
 *   task create ?-name name? ?-stacksize bytes? ?-priority pri? ?script?
 *       Create a new Tcl VM on a FreeRTOS task. Returns a task handle.
 *       If script is given, it's evaluated immediately in the new task.
 *
 *   task eval <handle> <script>
 *       Send a script to the task's interpreter for evaluation.
 *       Blocks until the result is returned.
 *
 *   task send <handle> <script>
 *       Send a script for asynchronous evaluation (fire-and-forget).
 *
 *   task delete <handle>
 *       Destroy the task and free its interpreter.
 *
 *   task restart <handle>
 *       Kill and restart a task using its original configuration.
 *
 *   task list
 *       List all active task handles.
 *
 *   task info <handle>
 *       Detailed info: name, state, restart count, circuit breaker status.
 *
 *   task self
 *       Return the handle of the current task (if running in a task VM).
 */

#include <string.h>
#include <stdlib.h>
#include "jim.h"
#include "jim-subcmd.h"
#include "jim-esp32.h"
#include "jim-esp32-task.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "jim-task";

task_slot_t task_slots[TASK_MAX_SLOTS] = { 0 };
SemaphoreHandle_t task_slots_mutex = NULL;

static void ensure_mutex(void)
{
    if (task_slots_mutex == NULL) {
        task_slots_mutex = xSemaphoreCreateMutex();
    }
}

static int find_free_slot(void)
{
    for (int i = 0; i < TASK_MAX_SLOTS; i++) {
        if (!task_slots[i].in_use) return i;
    }
    return -1;
}

/* ---------------------------------------------------------------------------
 * Task entry point - runs a Tcl interpreter in its own FreeRTOS task
 * ---------------------------------------------------------------------------*/

static void tcl_task_entry(void *param)
{
    int slot_idx = (int)(intptr_t)param;
    task_slot_t *slot = &task_slots[slot_idx];

    ESP_LOGI(TAG, "Tcl task '%s' starting (slot %d, restart #%d)",
             slot->name, slot_idx, slot->restart_count);

    /* Create a fresh interpreter for this task */
    Jim_Interp *interp = Jim_CreateInterp();
    Jim_RegisterCoreCommands(interp);
    Jim_InitStaticExtensions(interp);
    Jim_Esp32PlatformInit(interp);

    /* Store handle reference so the task can identify itself */
    Jim_SetVariableStrWithStr(interp, "task::self", slot->name);
    Jim_SetVariableStr(interp, "task::slot",
        Jim_NewIntObj(interp, slot_idx));

    /* Expose the message queue so other extensions (e.g. sleep) can reach this VM */
    Jim_SetAssocData(interp, "task.msg_queue", NULL, slot->msg_queue);

    slot->interp = interp;
    slot->state = TASK_STATE_RUNNING;
    slot->last_activity_us = esp_timer_get_time();

    /* Run init script if provided (use the retained copy) */
    if (slot->retained_script) {
        int ret = Jim_Eval(interp, slot->retained_script);
        if (ret == JIM_ERR) {
            const char *err = Jim_String(Jim_GetResult(interp));
            ESP_LOGE(TAG, "Task '%s' init script error: %s", slot->name, err);
        }
    }

    /* Message processing loop */
    task_msg_t msg;
    while (1) {
        if (xQueueReceive(slot->msg_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        slot->last_activity_us = esp_timer_get_time();

        if (msg.type == TASK_MSG_SHUTDOWN) {
            if (msg.script) free(msg.script);
            break;
        }

        if (msg.type == TASK_MSG_EVAL || msg.type == TASK_MSG_SEND) {
            int retcode = Jim_Eval(interp, msg.script);
            free(msg.script);

            if (msg.type == TASK_MSG_EVAL && msg.reply_queue) {
                /* Send result back */
                const char *result_str = Jim_String(Jim_GetResult(interp));
                task_reply_t reply;
                reply.retcode = retcode;
                reply.result = strdup(result_str);
                xQueueSend(msg.reply_queue, &reply, portMAX_DELAY);
            }
            else if (msg.type == TASK_MSG_SEND && retcode == JIM_ERR) {
                const char *err = Jim_String(Jim_GetResult(interp));
                ESP_LOGE(TAG, "Task '%s' async error: %s", slot->name, err);
            }
        }
    }

    /* Cleanup */
    ESP_LOGI(TAG, "Tcl task '%s' shutting down", slot->name);
    Jim_FreeInterp(interp);

    xSemaphoreTake(task_slots_mutex, portMAX_DELAY);
    slot->interp = NULL;
    slot->state = TASK_STATE_STOPPED;
    /* Don't free retained_script or reset in_use here — restart may reuse them */
    if (!slot->restart_pending) {
        /* Normal shutdown — fully clean up */
        slot->in_use = 0;
        vQueueDelete(slot->msg_queue);
        slot->msg_queue = NULL;
        if (slot->retained_script) {
            free(slot->retained_script);
            slot->retained_script = NULL;
        }
    }
    xSemaphoreGive(task_slots_mutex);

    vTaskDelete(NULL);
}

/* ---------------------------------------------------------------------------
 * Internal: start a task in a slot (used by create and restart)
 * ---------------------------------------------------------------------------*/

static int task_start_in_slot(int slot_idx)
{
    task_slot_t *slot = &task_slots[slot_idx];

    if (slot->msg_queue == NULL) {
        slot->msg_queue = xQueueCreate(TASK_MSG_QUEUE_LEN, sizeof(task_msg_t));
    } else {
        xQueueReset(slot->msg_queue);
    }

    slot->state = TASK_STATE_STARTING;
    slot->restart_pending = 0;

    BaseType_t ret = xTaskCreate(
        tcl_task_entry,
        slot->name,
        slot->stacksize,
        (void *)(intptr_t)slot_idx,
        (UBaseType_t)slot->priority,
        &slot->task_handle
    );

    if (ret != pdPASS) {
        slot->state = TASK_STATE_STOPPED;
        return -1;
    }

    return 0;
}

/* ---------------------------------------------------------------------------
 * Public API for other extensions (watchdog, sleep)
 * ---------------------------------------------------------------------------*/

int task_force_kill(int slot_idx)
{
    if (slot_idx < 0 || slot_idx >= TASK_MAX_SLOTS) return -1;
    task_slot_t *slot = &task_slots[slot_idx];
    if (!slot->in_use) return -1;

    /* Try graceful shutdown first */
    if (slot->msg_queue && slot->state == TASK_STATE_RUNNING) {
        task_msg_t msg = { .type = TASK_MSG_SHUTDOWN, .script = NULL, .reply_queue = NULL };
        if (xQueueSend(slot->msg_queue, &msg, pdMS_TO_TICKS(500)) == pdTRUE) {
            /* Wait briefly for graceful exit */
            vTaskDelay(pdMS_TO_TICKS(300));
        }
    }

    /* If still running, force kill the FreeRTOS task */
    if (slot->task_handle && slot->state != TASK_STATE_STOPPED) {
        ESP_LOGW(TAG, "Force-killing task '%s'", slot->name);
        vTaskDelete(slot->task_handle);
        slot->task_handle = NULL;
        slot->state = TASK_STATE_STOPPED;

        /* Clean up the interpreter if the task didn't get to */
        if (slot->interp) {
            Jim_FreeInterp(slot->interp);
            slot->interp = NULL;
        }
    }

    return 0;
}

int task_restart(int slot_idx)
{
    if (slot_idx < 0 || slot_idx >= TASK_MAX_SLOTS) return -1;
    task_slot_t *slot = &task_slots[slot_idx];
    if (!slot->in_use) return -1;

    /* Check circuit breaker */
    if (slot->cb_state == CB_OPEN) {
        int64_t now = esp_timer_get_time();
        int64_t cooldown = (int64_t)slot->cb_cooldown_ms * 1000;
        if ((now - slot->cb_open_since_us) < cooldown) {
            ESP_LOGE(TAG, "Circuit breaker OPEN for task '%s' — refusing restart (cooldown %lu ms remaining)",
                     slot->name,
                     (unsigned long)((cooldown - (now - slot->cb_open_since_us)) / 1000));
            return -2; /* Circuit breaker tripped */
        }
        /* Cooldown expired — move to half-open */
        slot->cb_state = CB_HALF_OPEN;
        ESP_LOGI(TAG, "Circuit breaker half-open for task '%s', allowing one restart", slot->name);
    }

    /* Mark restart pending so the shutdown handler preserves retained_script */
    slot->restart_pending = 1;

    /* Kill existing task */
    task_force_kill(slot_idx);

    /* Track restart for circuit breaker */
    int64_t now = esp_timer_get_time();
    slot->restart_count++;

    /* Shift restart timestamps */
    for (int i = CB_HISTORY_LEN - 1; i > 0; i--) {
        slot->restart_timestamps_us[i] = slot->restart_timestamps_us[i - 1];
    }
    slot->restart_timestamps_us[0] = now;

    /* Check if we've hit the failure threshold */
    int recent_restarts = 0;
    int64_t window = (int64_t)slot->cb_window_ms * 1000;
    for (int i = 0; i < CB_HISTORY_LEN; i++) {
        if (slot->restart_timestamps_us[i] > 0 &&
            (now - slot->restart_timestamps_us[i]) < window) {
            recent_restarts++;
        }
    }

    if (recent_restarts >= slot->cb_max_restarts) {
        slot->cb_state = CB_OPEN;
        slot->cb_open_since_us = now;
        ESP_LOGE(TAG, "Circuit breaker OPEN for task '%s': %d restarts in %lu ms window",
                 slot->name, recent_restarts, (unsigned long)slot->cb_window_ms);
        /* Don't start the task — breaker is open */
        return -2;
    }

    if (slot->cb_state == CB_HALF_OPEN) {
        /* Successful restart from half-open — close the breaker */
        slot->cb_state = CB_CLOSED;
        ESP_LOGI(TAG, "Circuit breaker closed for task '%s'", slot->name);
    }

    /* Start the task again */
    if (task_start_in_slot(slot_idx) != 0) {
        ESP_LOGE(TAG, "Failed to restart task '%s'", slot->name);
        return -1;
    }

    ESP_LOGI(TAG, "Task '%s' restarted (restart #%d)", slot->name, slot->restart_count);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Tcl commands
 * ---------------------------------------------------------------------------*/

static int task_cmd_create(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    ensure_mutex();

    const char *name = NULL;
    long stacksize = TASK_DEFAULT_STACK;
    long priority = TASK_DEFAULT_PRIORITY;
    const char *script = NULL;
    int i;

    /* Parse options */
    for (i = 0; i < argc; i++) {
        const char *arg = Jim_String(argv[i]);
        if (strcmp(arg, "-name") == 0 && i + 1 < argc) {
            name = Jim_String(argv[++i]);
        }
        else if (strcmp(arg, "-stacksize") == 0 && i + 1 < argc) {
            if (Jim_GetLong(interp, argv[++i], &stacksize) != JIM_OK) return JIM_ERR;
        }
        else if (strcmp(arg, "-priority") == 0 && i + 1 < argc) {
            if (Jim_GetLong(interp, argv[++i], &priority) != JIM_OK) return JIM_ERR;
        }
        else {
            /* Remaining arg is the init script */
            script = Jim_String(argv[i]);
        }
    }

    xSemaphoreTake(task_slots_mutex, portMAX_DELAY);
    int slot_idx = find_free_slot();
    if (slot_idx < 0) {
        xSemaphoreGive(task_slots_mutex);
        Jim_SetResultString(interp, "maximum number of Tcl tasks reached", -1);
        return JIM_ERR;
    }

    task_slot_t *slot = &task_slots[slot_idx];
    memset(slot, 0, sizeof(*slot));
    slot->in_use = 1;
    slot->stacksize = (uint32_t)stacksize;
    slot->priority = (UBaseType_t)priority;

    /* Circuit breaker defaults */
    slot->cb_state = CB_CLOSED;
    slot->cb_max_restarts = CB_DEFAULT_MAX_RESTARTS;
    slot->cb_window_ms = CB_DEFAULT_WINDOW_MS;
    slot->cb_cooldown_ms = CB_DEFAULT_COOLDOWN_MS;

    if (name) {
        strncpy(slot->name, name, sizeof(slot->name) - 1);
        slot->name[sizeof(slot->name) - 1] = '\0';
    } else {
        snprintf(slot->name, sizeof(slot->name), "tcl%d", slot_idx);
    }

    /* Retain a copy of the init script for restart */
    slot->retained_script = script ? strdup(script) : NULL;

    if (task_start_in_slot(slot_idx) != 0) {
        slot->in_use = 0;
        if (slot->retained_script) { free(slot->retained_script); slot->retained_script = NULL; }
        if (slot->msg_queue) { vQueueDelete(slot->msg_queue); slot->msg_queue = NULL; }
        xSemaphoreGive(task_slots_mutex);
        Jim_SetResultString(interp, "failed to create FreeRTOS task", -1);
        return JIM_ERR;
    }

    xSemaphoreGive(task_slots_mutex);

    Jim_SetResultInt(interp, slot_idx);
    return JIM_OK;
}

static int task_get_slot(Jim_Interp *interp, Jim_Obj *obj, task_slot_t **out)
{
    long idx;
    if (Jim_GetLong(interp, obj, &idx) != JIM_OK) return JIM_ERR;
    if (idx < 0 || idx >= TASK_MAX_SLOTS || !task_slots[idx].in_use) {
        Jim_SetResultFormatted(interp, "invalid task handle: %ld", idx);
        return JIM_ERR;
    }
    *out = &task_slots[idx];
    return JIM_OK;
}

static int task_cmd_eval(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    task_slot_t *slot;
    if (task_get_slot(interp, argv[0], &slot) != JIM_OK) return JIM_ERR;

    const char *script = Jim_String(argv[1]);

    /* Create a temporary reply queue */
    QueueHandle_t reply_queue = xQueueCreate(1, sizeof(task_reply_t));
    if (!reply_queue) {
        Jim_SetResultString(interp, "failed to create reply queue", -1);
        return JIM_ERR;
    }

    task_msg_t msg;
    msg.type = TASK_MSG_EVAL;
    msg.script = strdup(script);
    msg.reply_queue = reply_queue;

    if (xQueueSend(slot->msg_queue, &msg, pdMS_TO_TICKS(5000)) != pdTRUE) {
        free(msg.script);
        vQueueDelete(reply_queue);
        Jim_SetResultString(interp, "task message queue full", -1);
        return JIM_ERR;
    }

    /* Wait for reply */
    task_reply_t reply;
    if (xQueueReceive(reply_queue, &reply, pdMS_TO_TICKS(30000)) != pdTRUE) {
        vQueueDelete(reply_queue);
        Jim_SetResultString(interp, "timeout waiting for task reply", -1);
        return JIM_ERR;
    }

    vQueueDelete(reply_queue);

    if (reply.result) {
        Jim_SetResultString(interp, reply.result, -1);
        free(reply.result);
    }
    return reply.retcode;
}

static int task_cmd_send(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    task_slot_t *slot;
    if (task_get_slot(interp, argv[0], &slot) != JIM_OK) return JIM_ERR;

    const char *script = Jim_String(argv[1]);

    task_msg_t msg;
    msg.type = TASK_MSG_SEND;
    msg.script = strdup(script);
    msg.reply_queue = NULL;

    if (xQueueSend(slot->msg_queue, &msg, pdMS_TO_TICKS(5000)) != pdTRUE) {
        free(msg.script);
        Jim_SetResultString(interp, "task message queue full", -1);
        return JIM_ERR;
    }

    return JIM_OK;
}

static int task_cmd_delete(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    task_slot_t *slot;
    if (task_get_slot(interp, argv[0], &slot) != JIM_OK) return JIM_ERR;

    int slot_idx = (int)(slot - task_slots);
    task_force_kill(slot_idx);

    /* Full cleanup */
    xSemaphoreTake(task_slots_mutex, portMAX_DELAY);
    slot->in_use = 0;
    if (slot->msg_queue) { vQueueDelete(slot->msg_queue); slot->msg_queue = NULL; }
    if (slot->retained_script) { free(slot->retained_script); slot->retained_script = NULL; }
    xSemaphoreGive(task_slots_mutex);

    return JIM_OK;
}

static int task_cmd_restart(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    ensure_mutex();
    task_slot_t *slot;
    if (task_get_slot(interp, argv[0], &slot) != JIM_OK) return JIM_ERR;

    int slot_idx = (int)(slot - task_slots);
    int ret = task_restart(slot_idx);

    if (ret == -2) {
        Jim_SetResultFormatted(interp,
            "circuit breaker open for task '%s': too many restarts in window", slot->name);
        return JIM_ERR;
    }
    if (ret == -1) {
        Jim_SetResultString(interp, "failed to restart task", -1);
        return JIM_ERR;
    }

    Jim_SetResultInt(interp, slot_idx);
    return JIM_OK;
}

static int task_cmd_info(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    task_slot_t *slot;
    if (task_get_slot(interp, argv[0], &slot) != JIM_OK) return JIM_ERR;

    Jim_Obj *result = Jim_NewListObj(interp, NULL, 0);

    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "name", -1));
    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, slot->name, -1));

    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "state", -1));
    const char *state_str;
    switch (slot->state) {
        case TASK_STATE_STARTING: state_str = "starting"; break;
        case TASK_STATE_RUNNING:  state_str = "running"; break;
        case TASK_STATE_STOPPED:  state_str = "stopped"; break;
        default:                  state_str = "unknown";
    }
    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, state_str, -1));

    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "restarts", -1));
    Jim_ListAppendElement(interp, result, Jim_NewIntObj(interp, slot->restart_count));

    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "circuit_breaker", -1));
    const char *cb_str;
    switch (slot->cb_state) {
        case CB_CLOSED:    cb_str = "closed"; break;
        case CB_OPEN:      cb_str = "open"; break;
        case CB_HALF_OPEN: cb_str = "half-open"; break;
        default:           cb_str = "unknown";
    }
    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, cb_str, -1));

    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "last_activity_ms", -1));
    int64_t now = esp_timer_get_time();
    int64_t idle_ms = (now - slot->last_activity_us) / 1000;
    Jim_ListAppendElement(interp, result, Jim_NewIntObj(interp, (jim_wide)idle_ms));

    Jim_SetResult(interp, result);
    return JIM_OK;
}

static int task_cmd_list(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    ensure_mutex();

    Jim_Obj *result = Jim_NewListObj(interp, NULL, 0);

    xSemaphoreTake(task_slots_mutex, portMAX_DELAY);
    for (int i = 0; i < TASK_MAX_SLOTS; i++) {
        if (task_slots[i].in_use) {
            Jim_Obj *entry = Jim_NewListObj(interp, NULL, 0);
            Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, "id", -1));
            Jim_ListAppendElement(interp, entry, Jim_NewIntObj(interp, i));
            Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, "name", -1));
            Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, task_slots[i].name, -1));
            Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, "restarts", -1));
            Jim_ListAppendElement(interp, entry, Jim_NewIntObj(interp, task_slots[i].restart_count));
            Jim_ListAppendElement(interp, result, entry);
        }
    }
    xSemaphoreGive(task_slots_mutex);

    Jim_SetResult(interp, result);
    return JIM_OK;
}

static int task_cmd_self(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    Jim_Obj *slot_obj = Jim_GetVariableStr(interp, "task::slot", JIM_NONE);
    if (slot_obj) {
        Jim_SetResult(interp, slot_obj);
    } else {
        Jim_SetResultString(interp, "main", -1);
    }
    return JIM_OK;
}

static const jim_subcmd_type task_command_table[] = {
    {   "create",
        "?-name name? ?-stacksize bytes? ?-priority pri? ?script?",
        task_cmd_create,
        0,
        -1,
        /* Description: Create a new Tcl VM on a FreeRTOS task */
    },
    {   "eval",
        "handle script",
        task_cmd_eval,
        2,
        2,
        /* Description: Evaluate script in task VM (synchronous) */
    },
    {   "send",
        "handle script",
        task_cmd_send,
        2,
        2,
        /* Description: Send script to task VM (asynchronous) */
    },
    {   "delete",
        "handle",
        task_cmd_delete,
        1,
        1,
        /* Description: Destroy a task VM */
    },
    {   "restart",
        "handle",
        task_cmd_restart,
        1,
        1,
        /* Description: Kill and restart a task VM (subject to circuit breaker) */
    },
    {   "info",
        "handle",
        task_cmd_info,
        1,
        1,
        /* Description: Show task state, restart count, circuit breaker status */
    },
    {   "list",
        NULL,
        task_cmd_list,
        0,
        0,
        /* Description: List all active Tcl task VMs */
    },
    {   "self",
        NULL,
        task_cmd_self,
        0,
        0,
        /* Description: Return handle of current task (or 'main') */
    },
    { NULL }
};

int Jim_esp_taskInit(Jim_Interp *interp)
{
    Jim_PackageProvideCheck(interp, "esp_task");
    Jim_RegisterSubCmd(interp, "task", task_command_table, NULL);
    return JIM_OK;
}
