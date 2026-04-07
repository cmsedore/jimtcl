/* Jim Tcl Cron/Scheduler Extension for ESP32
 *
 * Periodic and one-shot script execution via FreeRTOS timers and
 * task message delivery.
 *
 *   cron add <interval_ms> -callback {proc task} ?-name name?
 *   cron remove <id|name>
 *   cron list
 *   cron once <delay_ms> -callback {proc task}
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "jim.h"
#include "jim-subcmd.h"
#include "jim-esp32-task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "esp_log.h"

static const char *TAG = "jim-cron";

/* -----------------------------------------------------------------------
 * Cron slot table
 * ----------------------------------------------------------------------- */

#define CRON_MAX_ENTRIES 16

typedef struct {
    int active;
    char name[32];
    long interval_ms;
    TimerHandle_t timer;
    char callback_proc[64];
    char callback_target[16];
    int one_shot;
} cron_entry_t;

static cron_entry_t cron_entries[CRON_MAX_ENTRIES] = { 0 };

/* -----------------------------------------------------------------------
 * Timer callback -- fires from the FreeRTOS timer daemon task.
 * Delivers the callback proc string to the target Tcl task.
 * ----------------------------------------------------------------------- */

static void cron_timer_callback(TimerHandle_t xTimer)
{
    int slot = (int)(intptr_t)pvTimerGetTimerID(xTimer);
    if (slot < 0 || slot >= CRON_MAX_ENTRIES) return;

    cron_entry_t *entry = &cron_entries[slot];
    if (!entry->active) return;

    if (task_send_to_name(entry->callback_target, entry->callback_proc) != 0) {
        ESP_LOGW(TAG, "cron delivery failed: slot %d -> task '%s'",
                 slot, entry->callback_target);
    }

    /* For one-shot timers, mark the slot inactive after firing */
    if (entry->one_shot) {
        entry->active = 0;
        /* Timer auto-stops because it was created with pdFALSE auto-reload */
    }
}

/* Find a free slot, return index or -1 */
static int cron_find_free_slot(void)
{
    for (int i = 0; i < CRON_MAX_ENTRIES; i++) {
        if (!cron_entries[i].active) return i;
    }
    return -1;
}

/* Find a slot by name, return index or -1 */
static int cron_find_by_name(const char *name)
{
    for (int i = 0; i < CRON_MAX_ENTRIES; i++) {
        if (cron_entries[i].active && strcmp(cron_entries[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/* -----------------------------------------------------------------------
 * Tcl subcommands
 * ----------------------------------------------------------------------- */

static int cron_cmd_add(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    /* argv[0] = interval_ms, then keyword args */
    long interval_ms;
    if (Jim_GetLong(interp, argv[0], &interval_ms) != JIM_OK) {
        return JIM_ERR;
    }
    if (interval_ms <= 0) {
        Jim_SetResultString(interp, "interval_ms must be positive", -1);
        return JIM_ERR;
    }

    const char *cb_proc = NULL;
    const char *cb_target = NULL;
    const char *name = NULL;

    for (int i = 1; i < argc; i++) {
        const char *opt = Jim_String(argv[i]);
        if (strcmp(opt, "-callback") == 0 && i + 1 < argc) {
            Jim_Obj *cbObj = argv[++i];
            if (Jim_ListLength(interp, cbObj) != 2) {
                Jim_SetResultString(interp, "-callback requires {procname target_task}", -1);
                return JIM_ERR;
            }
            cb_proc = Jim_String(Jim_ListGetIndex(interp, cbObj, 0));
            cb_target = Jim_String(Jim_ListGetIndex(interp, cbObj, 1));
        } else if (strcmp(opt, "-name") == 0 && i + 1 < argc) {
            name = Jim_String(argv[++i]);
        } else {
            Jim_SetResultFormatted(interp, "unknown option \"%s\"", opt);
            return JIM_ERR;
        }
    }

    if (!cb_proc || !cb_target) {
        Jim_SetResultString(interp, "must specify -callback {procname target_task}", -1);
        return JIM_ERR;
    }

    int slot = cron_find_free_slot();
    if (slot < 0) {
        Jim_SetResultString(interp, "maximum cron entries reached", -1);
        return JIM_ERR;
    }

    cron_entry_t *entry = &cron_entries[slot];
    memset(entry, 0, sizeof(*entry));
    entry->active = 1;
    entry->interval_ms = interval_ms;
    entry->one_shot = 0;

    if (name) {
        strncpy(entry->name, name, sizeof(entry->name) - 1);
    } else {
        snprintf(entry->name, sizeof(entry->name), "cron_%d", slot);
    }

    strncpy(entry->callback_proc, cb_proc, sizeof(entry->callback_proc) - 1);
    entry->callback_proc[sizeof(entry->callback_proc) - 1] = '\0';
    strncpy(entry->callback_target, cb_target, sizeof(entry->callback_target) - 1);
    entry->callback_target[sizeof(entry->callback_target) - 1] = '\0';

    /* Create auto-reload timer */
    entry->timer = xTimerCreate(
        entry->name,
        pdMS_TO_TICKS(interval_ms),
        pdTRUE,                           /* Auto-reload */
        (void *)(intptr_t)slot,           /* Timer ID = slot index */
        cron_timer_callback
    );

    if (entry->timer == NULL) {
        entry->active = 0;
        Jim_SetResultString(interp, "failed to create FreeRTOS timer", -1);
        return JIM_ERR;
    }

    if (xTimerStart(entry->timer, pdMS_TO_TICKS(100)) != pdPASS) {
        xTimerDelete(entry->timer, pdMS_TO_TICKS(100));
        entry->active = 0;
        Jim_SetResultString(interp, "failed to start timer", -1);
        return JIM_ERR;
    }

    ESP_LOGI(TAG, "cron add: slot %d name='%s' interval=%ldms -> %s in task '%s'",
             slot, entry->name, interval_ms, cb_proc, cb_target);

    Jim_SetResultInt(interp, slot);
    return JIM_OK;
}

static int cron_cmd_once(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    /* argv[0] = delay_ms, then keyword args */
    long delay_ms;
    if (Jim_GetLong(interp, argv[0], &delay_ms) != JIM_OK) {
        return JIM_ERR;
    }
    if (delay_ms <= 0) {
        Jim_SetResultString(interp, "delay_ms must be positive", -1);
        return JIM_ERR;
    }

    const char *cb_proc = NULL;
    const char *cb_target = NULL;

    for (int i = 1; i < argc; i++) {
        const char *opt = Jim_String(argv[i]);
        if (strcmp(opt, "-callback") == 0 && i + 1 < argc) {
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

    if (!cb_proc || !cb_target) {
        Jim_SetResultString(interp, "must specify -callback {procname target_task}", -1);
        return JIM_ERR;
    }

    int slot = cron_find_free_slot();
    if (slot < 0) {
        Jim_SetResultString(interp, "maximum cron entries reached", -1);
        return JIM_ERR;
    }

    cron_entry_t *entry = &cron_entries[slot];
    memset(entry, 0, sizeof(*entry));
    entry->active = 1;
    entry->interval_ms = delay_ms;
    entry->one_shot = 1;

    snprintf(entry->name, sizeof(entry->name), "once_%d", slot);

    strncpy(entry->callback_proc, cb_proc, sizeof(entry->callback_proc) - 1);
    entry->callback_proc[sizeof(entry->callback_proc) - 1] = '\0';
    strncpy(entry->callback_target, cb_target, sizeof(entry->callback_target) - 1);
    entry->callback_target[sizeof(entry->callback_target) - 1] = '\0';

    /* Create one-shot timer (pdFALSE = no auto-reload) */
    entry->timer = xTimerCreate(
        entry->name,
        pdMS_TO_TICKS(delay_ms),
        pdFALSE,                          /* One-shot */
        (void *)(intptr_t)slot,
        cron_timer_callback
    );

    if (entry->timer == NULL) {
        entry->active = 0;
        Jim_SetResultString(interp, "failed to create FreeRTOS timer", -1);
        return JIM_ERR;
    }

    if (xTimerStart(entry->timer, pdMS_TO_TICKS(100)) != pdPASS) {
        xTimerDelete(entry->timer, pdMS_TO_TICKS(100));
        entry->active = 0;
        Jim_SetResultString(interp, "failed to start timer", -1);
        return JIM_ERR;
    }

    ESP_LOGI(TAG, "cron once: slot %d delay=%ldms -> %s in task '%s'",
             slot, delay_ms, cb_proc, cb_target);

    Jim_SetResultInt(interp, slot);
    return JIM_OK;
}

static int cron_cmd_remove(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    const char *id_str = Jim_String(argv[0]);
    int slot = -1;

    /* Try numeric slot index first */
    long id_val;
    if (Jim_GetLong(interp, argv[0], &id_val) == JIM_OK) {
        if (id_val >= 0 && id_val < CRON_MAX_ENTRIES && cron_entries[id_val].active) {
            slot = (int)id_val;
        }
    }

    /* Fall back to name lookup */
    if (slot < 0) {
        slot = cron_find_by_name(id_str);
    }

    if (slot < 0) {
        Jim_SetResultFormatted(interp, "cron entry not found: \"%s\"", id_str);
        return JIM_ERR;
    }

    cron_entry_t *entry = &cron_entries[slot];

    if (entry->timer != NULL) {
        xTimerStop(entry->timer, pdMS_TO_TICKS(100));
        xTimerDelete(entry->timer, pdMS_TO_TICKS(100));
        entry->timer = NULL;
    }

    entry->active = 0;
    ESP_LOGI(TAG, "cron remove: slot %d name='%s'", slot, entry->name);

    return JIM_OK;
}

static int cron_cmd_list(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    Jim_Obj *result = Jim_NewListObj(interp, NULL, 0);

    for (int i = 0; i < CRON_MAX_ENTRIES; i++) {
        cron_entry_t *entry = &cron_entries[i];
        if (!entry->active) continue;

        Jim_Obj *dict = Jim_NewListObj(interp, NULL, 0);

        Jim_ListAppendElement(interp, dict, Jim_NewStringObj(interp, "id", -1));
        Jim_ListAppendElement(interp, dict, Jim_NewIntObj(interp, i));

        Jim_ListAppendElement(interp, dict, Jim_NewStringObj(interp, "name", -1));
        Jim_ListAppendElement(interp, dict, Jim_NewStringObj(interp, entry->name, -1));

        Jim_ListAppendElement(interp, dict, Jim_NewStringObj(interp, "interval_ms", -1));
        Jim_ListAppendElement(interp, dict, Jim_NewIntObj(interp, entry->interval_ms));

        Jim_ListAppendElement(interp, dict, Jim_NewStringObj(interp, "target", -1));
        Jim_ListAppendElement(interp, dict, Jim_NewStringObj(interp, entry->callback_target, -1));

        Jim_ListAppendElement(interp, dict, Jim_NewStringObj(interp, "callback", -1));
        Jim_ListAppendElement(interp, dict, Jim_NewStringObj(interp, entry->callback_proc, -1));

        Jim_ListAppendElement(interp, dict, Jim_NewStringObj(interp, "one_shot", -1));
        Jim_ListAppendElement(interp, dict, Jim_NewIntObj(interp, entry->one_shot));

        Jim_ListAppendElement(interp, result, dict);
    }

    Jim_SetResult(interp, result);
    return JIM_OK;
}

/* -----------------------------------------------------------------------
 * Subcommand dispatch table
 * ----------------------------------------------------------------------- */

static const jim_subcmd_type cron_command_table[] = {
    {   "add",
        "interval_ms -callback {proc task} ?-name name?",
        cron_cmd_add,
        3,
        -1,
        /* Description: Add a periodic timer that delivers a callback */
    },
    {   "once",
        "delay_ms -callback {proc task}",
        cron_cmd_once,
        3,
        -1,
        /* Description: Schedule a one-shot delayed callback */
    },
    {   "remove",
        "id|name",
        cron_cmd_remove,
        1,
        1,
        /* Description: Stop and remove a cron entry by id or name */
    },
    {   "list",
        "",
        cron_cmd_list,
        0,
        0,
        /* Description: List all active cron entries */
    },
    { NULL }
};

int Jim_cronInit(Jim_Interp *interp)
{
    Jim_PackageProvideCheck(interp, "cron");
    Jim_RegisterSubCmd(interp, "cron", cron_command_table, NULL);
    return JIM_OK;
}
