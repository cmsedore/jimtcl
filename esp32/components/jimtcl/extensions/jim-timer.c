/* Jim Tcl Timer Extension for ESP32
 *
 * Provides Tcl commands for FreeRTOS software timers that deliver
 * callbacks to named Tcl task VMs:
 *
 *   timer create <ms> -callback {proc task} ?-repeat? ?-name name?
 *   timer cancel <id|name>
 *   timer list
 */

#include <string.h>
#include <stdlib.h>
#include "jim.h"
#include "jim-subcmd.h"
#include "jim-esp32-task.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

static const char *TAG = "jim-timer";

#define TIMER_MAX_SLOTS 8

typedef struct {
    int active;
    char name[32];
    TimerHandle_t handle;
    char callback_proc[64];
    char callback_target[16];
    int repeat;
    uint32_t interval_ms;
} timer_slot_t;

static timer_slot_t timer_slots[TIMER_MAX_SLOTS] = { 0 };

/* FreeRTOS timer callback -- runs from timer daemon task.
 * Sends the callback script to the target Tcl task via task_send_to_name. */
static void timer_callback(TimerHandle_t xTimer)
{
    int slot = (int)(intptr_t)pvTimerGetTimerID(xTimer);
    if (slot < 0 || slot >= TIMER_MAX_SLOTS) return;

    timer_slot_t *t = &timer_slots[slot];
    if (!t->active) return;

    if (task_send_to_name(t->callback_target, t->callback_proc) != 0) {
        ESP_LOGW(TAG, "Timer '%s' callback delivery failed -> task '%s'",
                 t->name, t->callback_target);
    }

    /* For one-shot timers, mark inactive after firing */
    if (!t->repeat) {
        t->active = 0;
        /* Timer has already stopped itself (auto-reload was pdFALSE) */
    }
}

static int timer_find_by_name(const char *name)
{
    for (int i = 0; i < TIMER_MAX_SLOTS; i++) {
        if (timer_slots[i].active && strcmp(timer_slots[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static int timer_find_free(void)
{
    for (int i = 0; i < TIMER_MAX_SLOTS; i++) {
        if (!timer_slots[i].active) return i;
    }
    return -1;
}

static int timer_cmd_create(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    long ms;
    if (Jim_GetLong(interp, argv[0], &ms) != JIM_OK) {
        return JIM_ERR;
    }
    if (ms <= 0) {
        Jim_SetResultString(interp, "interval must be positive", -1);
        return JIM_ERR;
    }

    const char *cb_proc = NULL;
    const char *cb_target = NULL;
    const char *name = NULL;
    int repeat = 0;

    /* Parse keyword arguments */
    for (int i = 1; i < argc; i++) {
        const char *opt = Jim_String(argv[i]);

        if (strcmp(opt, "-callback") == 0) {
            if (i + 1 >= argc) {
                Jim_SetResultString(interp, "missing value for -callback", -1);
                return JIM_ERR;
            }
            Jim_Obj *cbObj = argv[++i];
            if (Jim_ListLength(interp, cbObj) != 2) {
                Jim_SetResultString(interp, "-callback requires {procname target_task}", -1);
                return JIM_ERR;
            }
            cb_proc = Jim_String(Jim_ListGetIndex(interp, cbObj, 0));
            cb_target = Jim_String(Jim_ListGetIndex(interp, cbObj, 1));
        } else if (strcmp(opt, "-repeat") == 0) {
            repeat = 1;
        } else if (strcmp(opt, "-name") == 0) {
            if (i + 1 >= argc) {
                Jim_SetResultString(interp, "missing value for -name", -1);
                return JIM_ERR;
            }
            name = Jim_String(argv[++i]);
        } else {
            Jim_SetResultFormatted(interp, "unknown option \"%s\"", opt);
            return JIM_ERR;
        }
    }

    if (!cb_proc || !cb_target) {
        Jim_SetResultString(interp, "must specify -callback {proc task}", -1);
        return JIM_ERR;
    }

    /* Check for duplicate name */
    if (name && timer_find_by_name(name) >= 0) {
        Jim_SetResultFormatted(interp, "timer with name \"%s\" already exists", name);
        return JIM_ERR;
    }

    int slot = timer_find_free();
    if (slot < 0) {
        Jim_SetResultString(interp, "maximum number of timers reached", -1);
        return JIM_ERR;
    }

    timer_slot_t *t = &timer_slots[slot];

    /* Set name -- use provided name or generate one from slot index */
    if (name) {
        strncpy(t->name, name, sizeof(t->name) - 1);
        t->name[sizeof(t->name) - 1] = '\0';
    } else {
        snprintf(t->name, sizeof(t->name), "timer%d", slot);
    }

    strncpy(t->callback_proc, cb_proc, sizeof(t->callback_proc) - 1);
    t->callback_proc[sizeof(t->callback_proc) - 1] = '\0';
    strncpy(t->callback_target, cb_target, sizeof(t->callback_target) - 1);
    t->callback_target[sizeof(t->callback_target) - 1] = '\0';
    t->repeat = repeat;
    t->interval_ms = (uint32_t)ms;

    /* Create FreeRTOS timer */
    t->handle = xTimerCreate(
        t->name,
        pdMS_TO_TICKS(ms),
        repeat ? pdTRUE : pdFALSE,  /* auto-reload */
        (void *)(intptr_t)slot,       /* timer ID = slot index */
        timer_callback
    );

    if (t->handle == NULL) {
        Jim_SetResultString(interp, "xTimerCreate failed", -1);
        return JIM_ERR;
    }

    t->active = 1;

    /* Start the timer immediately */
    if (xTimerStart(t->handle, pdMS_TO_TICKS(100)) != pdPASS) {
        xTimerDelete(t->handle, pdMS_TO_TICKS(100));
        t->active = 0;
        t->handle = NULL;
        Jim_SetResultString(interp, "xTimerStart failed", -1);
        return JIM_ERR;
    }

    ESP_LOGI(TAG, "Timer '%s' created: %ldms %s -> %s in task '%s'",
             t->name, ms, repeat ? "repeat" : "oneshot", cb_proc, cb_target);

    Jim_SetResultInt(interp, slot);
    return JIM_OK;
}

static int timer_cmd_cancel(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    const char *id_str = Jim_String(argv[0]);
    int slot = -1;

    /* Try as numeric index first */
    long idx;
    if (Jim_GetLong(interp, argv[0], &idx) == JIM_OK) {
        if (idx >= 0 && idx < TIMER_MAX_SLOTS && timer_slots[idx].active) {
            slot = (int)idx;
        }
    }

    /* Try as name */
    if (slot < 0) {
        slot = timer_find_by_name(id_str);
    }

    if (slot < 0) {
        Jim_SetResultFormatted(interp, "timer \"%s\" not found", id_str);
        return JIM_ERR;
    }

    timer_slot_t *t = &timer_slots[slot];

    xTimerStop(t->handle, pdMS_TO_TICKS(100));
    xTimerDelete(t->handle, pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "Timer '%s' cancelled", t->name);

    t->active = 0;
    t->handle = NULL;

    return JIM_OK;
}

static int timer_cmd_list(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    Jim_Obj *result = Jim_NewListObj(interp, NULL, 0);

    for (int i = 0; i < TIMER_MAX_SLOTS; i++) {
        timer_slot_t *t = &timer_slots[i];
        if (!t->active) continue;

        Jim_Obj *entry = Jim_NewListObj(interp, NULL, 0);
        Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, "id", -1));
        Jim_ListAppendElement(interp, entry, Jim_NewIntObj(interp, i));
        Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, "name", -1));
        Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, t->name, -1));
        Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, "interval", -1));
        Jim_ListAppendElement(interp, entry, Jim_NewIntObj(interp, (long)t->interval_ms));
        Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, "repeat", -1));
        Jim_ListAppendElement(interp, entry, Jim_NewIntObj(interp, t->repeat));
        Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, "callback", -1));
        Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, t->callback_proc, -1));
        Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, "target", -1));
        Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, t->callback_target, -1));
        Jim_ListAppendElement(interp, result, entry);
    }

    Jim_SetResult(interp, result);
    return JIM_OK;
}

static const jim_subcmd_type timer_command_table[] = {
    {   "create",
        "ms -callback {proc task} ?-repeat? ?-name name?",
        timer_cmd_create,
        1,
        -1,
        /* Description: Create a FreeRTOS software timer */
    },
    {   "cancel",
        "id|name",
        timer_cmd_cancel,
        1,
        1,
        /* Description: Cancel and delete a timer */
    },
    {   "list",
        "",
        timer_cmd_list,
        0,
        0,
        /* Description: List active timers */
    },
    { NULL }
};

int Jim_timerInit(Jim_Interp *interp)
{
    Jim_PackageProvideCheck(interp, "timer");
    Jim_RegisterSubCmd(interp, "timer", timer_command_table, NULL);
    return JIM_OK;
}
