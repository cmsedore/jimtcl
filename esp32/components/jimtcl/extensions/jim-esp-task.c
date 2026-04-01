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
 *   task list
 *       List all active task handles.
 *
 *   task self
 *       Return the handle of the current task (if running in a task VM).
 */

#include <string.h>
#include <stdlib.h>
#include "jim.h"
#include "jim-subcmd.h"
#include "jim-esp32.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "jim-task";

#define TASK_MAX_SLOTS 8
#define TASK_MSG_QUEUE_LEN 4
#define TASK_DEFAULT_STACK 8192
#define TASK_DEFAULT_PRIORITY 5

/* Message types sent to a task's interpreter */
typedef enum {
    TASK_MSG_EVAL,       /* Evaluate script, send result back */
    TASK_MSG_SEND,       /* Evaluate script, no result needed */
    TASK_MSG_SHUTDOWN,   /* Destroy interpreter and exit task */
} task_msg_type_t;

/* Message structure passed via queue */
typedef struct {
    task_msg_type_t type;
    char *script;                /* Heap-allocated script string (freed by receiver) */
    QueueHandle_t reply_queue;   /* For EVAL: queue to send result back on */
} task_msg_t;

/* Reply from a task eval */
typedef struct {
    int retcode;
    char *result;   /* Heap-allocated result string (freed by caller) */
} task_reply_t;

/* Per-task state */
typedef struct {
    int in_use;
    TaskHandle_t task_handle;
    QueueHandle_t msg_queue;
    char name[16];
    Jim_Interp *interp;         /* The task's own interpreter */
    char *init_script;          /* Optional script to run at startup */
} task_slot_t;

static task_slot_t task_slots[TASK_MAX_SLOTS] = { 0 };
static SemaphoreHandle_t slots_mutex = NULL;

static void ensure_mutex(void)
{
    if (slots_mutex == NULL) {
        slots_mutex = xSemaphoreCreateMutex();
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

    ESP_LOGI(TAG, "Tcl task '%s' starting (slot %d)", slot->name, slot_idx);

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

    /* Run init script if provided */
    if (slot->init_script) {
        int ret = Jim_Eval(interp, slot->init_script);
        if (ret == JIM_ERR) {
            const char *err = Jim_String(Jim_GetResult(interp));
            ESP_LOGE(TAG, "Task '%s' init script error: %s", slot->name, err);
        }
        free(slot->init_script);
        slot->init_script = NULL;
    }

    /* Message processing loop */
    task_msg_t msg;
    while (1) {
        if (xQueueReceive(slot->msg_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }

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

    xSemaphoreTake(slots_mutex, portMAX_DELAY);
    slot->interp = NULL;
    slot->in_use = 0;
    vQueueDelete(slot->msg_queue);
    slot->msg_queue = NULL;
    xSemaphoreGive(slots_mutex);

    vTaskDelete(NULL);
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

    xSemaphoreTake(slots_mutex, portMAX_DELAY);
    int slot_idx = find_free_slot();
    if (slot_idx < 0) {
        xSemaphoreGive(slots_mutex);
        Jim_SetResultString(interp, "maximum number of Tcl tasks reached", -1);
        return JIM_ERR;
    }

    task_slot_t *slot = &task_slots[slot_idx];
    slot->in_use = 1;

    if (name) {
        strncpy(slot->name, name, sizeof(slot->name) - 1);
        slot->name[sizeof(slot->name) - 1] = '\0';
    } else {
        snprintf(slot->name, sizeof(slot->name), "tcl%d", slot_idx);
    }

    slot->msg_queue = xQueueCreate(TASK_MSG_QUEUE_LEN, sizeof(task_msg_t));
    slot->init_script = script ? strdup(script) : NULL;
    slot->interp = NULL;

    BaseType_t ret = xTaskCreate(
        tcl_task_entry,
        slot->name,
        (uint32_t)stacksize,
        (void *)(intptr_t)slot_idx,
        (UBaseType_t)priority,
        &slot->task_handle
    );

    xSemaphoreGive(slots_mutex);

    if (ret != pdPASS) {
        slot->in_use = 0;
        if (slot->init_script) { free(slot->init_script); slot->init_script = NULL; }
        vQueueDelete(slot->msg_queue);
        Jim_SetResultString(interp, "failed to create FreeRTOS task", -1);
        return JIM_ERR;
    }

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

    task_msg_t msg;
    msg.type = TASK_MSG_SHUTDOWN;
    msg.script = NULL;
    msg.reply_queue = NULL;

    xQueueSend(slot->msg_queue, &msg, pdMS_TO_TICKS(5000));

    /* Give the task time to clean up */
    vTaskDelay(pdMS_TO_TICKS(500));
    return JIM_OK;
}

static int task_cmd_list(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    ensure_mutex();

    Jim_Obj *result = Jim_NewListObj(interp, NULL, 0);

    xSemaphoreTake(slots_mutex, portMAX_DELAY);
    for (int i = 0; i < TASK_MAX_SLOTS; i++) {
        if (task_slots[i].in_use) {
            Jim_Obj *entry = Jim_NewListObj(interp, NULL, 0);
            Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, "id", -1));
            Jim_ListAppendElement(interp, entry, Jim_NewIntObj(interp, i));
            Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, "name", -1));
            Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, task_slots[i].name, -1));
            Jim_ListAppendElement(interp, result, entry);
        }
    }
    xSemaphoreGive(slots_mutex);

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
