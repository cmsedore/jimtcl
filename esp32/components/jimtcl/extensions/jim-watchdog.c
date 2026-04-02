/* Jim Tcl ESP32 Watchdog Extension
 *
 * Monitors Tcl VM responsiveness and enforces system health for IoT devices
 * that must reliably enter sleep to preserve battery life.
 *
 * Architecture:
 *   - A dedicated FreeRTOS timer periodically pings all monitored VMs
 *   - If a VM doesn't respond within the deadline, it's marked unresponsive
 *   - Escalation path: warn → restart VM → circuit breaker → force sleep
 *   - The watchdog can force sleep bypassing the normal vote, for battery-critical situations
 *
 * Commands:
 *
 *   watchdog enable ?-interval ms? ?-deadline ms?
 *       Start the watchdog timer. Default interval=10000 (10s), deadline=5000 (5s).
 *
 *   watchdog disable
 *       Stop the watchdog timer.
 *
 *   watchdog kick
 *       VMs call this to prove liveness. Automatically called when processing
 *       messages, but long-running scripts should call this periodically.
 *
 *   watchdog monitor <task_handle>
 *       Add a task VM to the watchdog's monitoring list.
 *
 *   watchdog unmonitor <task_handle>
 *       Remove a task VM from monitoring.
 *
 *   watchdog policy ?-max_restarts n? ?-window_ms ms? ?-cooldown_ms ms?
 *       Configure the circuit breaker for a monitored task.
 *
 *   watchdog force_sleep light|deep
 *       Emergency: skip voting and immediately enter sleep.
 *       Use when battery is critically low.
 *
 *   watchdog status
 *       Return watchdog state and health of all monitored VMs.
 *
 *   watchdog oncritical <proc>
 *       Register a Tcl proc to be called in the main interpreter when
 *       a critical condition occurs (VM unresponsive, circuit breaker open).
 *       Proc receives: oncritical <event_type> <task_name> <details>
 */

#include <string.h>
#include <stdlib.h>
#include "jim.h"
#include "jim-subcmd.h"
#include "jim-esp32-task.h"
#include "jim-esp32-sleep.h"

#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_log.h"

static const char *TAG = "jim-watchdog";

/* Maximum tasks the watchdog can monitor */
#define WD_MAX_MONITORED 8

/* Monitored task entry */
typedef struct {
    int active;
    int task_slot;                  /* Index into task_slots[] */
    int64_t last_ping_sent_us;      /* When we last sent a health check */
    int64_t last_pong_us;           /* When the task last responded */
    int consecutive_failures;       /* How many pings went unanswered */
    int restarted_by_watchdog;      /* Count of watchdog-initiated restarts */
    QueueHandle_t reply_queue;      /* Persistent reply queue for health checks */
} wd_monitored_t;

/* Watchdog state */
typedef struct {
    int enabled;
    TimerHandle_t timer;
    SemaphoreHandle_t mutex;
    unsigned long interval_ms;      /* How often to check (default 10s) */
    unsigned long deadline_ms;      /* Max time to wait for pong (default 5s) */
    wd_monitored_t monitored[WD_MAX_MONITORED];
    char critical_proc[64];         /* Tcl proc for critical events */
    Jim_Interp *main_interp;        /* Main interp for critical callbacks */
} watchdog_state_t;

static watchdog_state_t wd = { 0 };

static void ensure_mutex(void)
{
    if (wd.mutex == NULL) {
        wd.mutex = xSemaphoreCreateMutex();
    }
}

/* ---------------------------------------------------------------------------
 * Health check: ping each monitored VM and evaluate responsiveness
 * ---------------------------------------------------------------------------*/

static void watchdog_check_task(wd_monitored_t *mon)
{
    if (!mon->active) return;

    int slot = mon->task_slot;
    if (slot < 0 || slot >= TASK_MAX_SLOTS) return;

    task_slot_t *ts = &task_slots[slot];
    if (!ts->in_use || ts->state != TASK_STATE_RUNNING || !ts->msg_queue) {
        return;
    }

    /* Use persistent reply queue; create on first use */
    if (!mon->reply_queue) {
        mon->reply_queue = xQueueCreate(1, sizeof(task_reply_t));
        if (!mon->reply_queue) return;
    }

    /* Drain any stale reply from a previous timed-out ping */
    task_reply_t stale;
    while (xQueueReceive(mon->reply_queue, &stale, 0) == pdTRUE) {
        if (stale.result) free(stale.result);
    }

    task_msg_t msg;
    msg.type = TASK_MSG_EVAL;
    msg.script = strdup("watchdog kick; return ok");
    msg.reply_queue = mon->reply_queue;

    mon->last_ping_sent_us = esp_timer_get_time();

    if (xQueueSend(ts->msg_queue, &msg, pdMS_TO_TICKS(500)) != pdTRUE) {
        /* Can't even queue the message — task is stuck */
        free(msg.script);
        mon->consecutive_failures++;
        ESP_LOGW(TAG, "Task '%s': queue full (failure #%d)", ts->name, mon->consecutive_failures);
        return;
    }

    /* Wait for response with deadline */
    task_reply_t reply;
    if (xQueueReceive(mon->reply_queue, &reply, pdMS_TO_TICKS(wd.deadline_ms)) == pdTRUE) {
        /* Task responded */
        mon->last_pong_us = esp_timer_get_time();
        mon->consecutive_failures = 0;
        if (reply.result) free(reply.result);
        return;
    }

    /* Timeout — task is unresponsive. Reply queue stays valid for next check. */
    mon->consecutive_failures++;

    ESP_LOGW(TAG, "Task '%s': unresponsive (failure #%d, deadline %lu ms)",
             ts->name, mon->consecutive_failures, wd.deadline_ms);

    /* Escalation logic */
    if (mon->consecutive_failures >= 3) {
        /* Try to restart the task */
        ESP_LOGW(TAG, "Task '%s': %d consecutive failures, attempting restart",
                 ts->name, mon->consecutive_failures);

        int ret = task_restart(slot);
        if (ret == 0) {
            mon->consecutive_failures = 0;
            mon->restarted_by_watchdog++;
            ESP_LOGI(TAG, "Task '%s' restarted by watchdog (total wd restarts: %d)",
                     ts->name, mon->restarted_by_watchdog);

            /* Notify via critical callback */
            if (wd.critical_proc[0] && wd.main_interp) {
                char script[128];
                snprintf(script, sizeof(script), "%s restart %s {watchdog restart after %d failures}",
                         wd.critical_proc, ts->name, mon->consecutive_failures);
                Jim_Eval(wd.main_interp, script);
            }
        }
        else if (ret == -2) {
            /* Circuit breaker open */
            ESP_LOGE(TAG, "Task '%s': circuit breaker OPEN — not restarting", ts->name);

            if (wd.critical_proc[0] && wd.main_interp) {
                char script[128];
                snprintf(script, sizeof(script), "%s breaker_open %s {circuit breaker tripped}",
                         wd.critical_proc, ts->name);
                Jim_Eval(wd.main_interp, script);
            }
        }
    }
    else if (mon->consecutive_failures >= 1) {
        /* First warning — notify but don't restart yet */
        if (wd.critical_proc[0] && wd.main_interp) {
            char script[128];
            snprintf(script, sizeof(script), "%s unresponsive %s {failure %d of 3}",
                     wd.critical_proc, ts->name, mon->consecutive_failures);
            Jim_Eval(wd.main_interp, script);
        }
    }
}

static void watchdog_timer_callback(TimerHandle_t timer)
{
    if (!wd.enabled) return;

    xSemaphoreTake(wd.mutex, portMAX_DELAY);
    for (int i = 0; i < WD_MAX_MONITORED; i++) {
        watchdog_check_task(&wd.monitored[i]);
    }
    xSemaphoreGive(wd.mutex);
}

/* ---------------------------------------------------------------------------
 * Tcl commands
 * ---------------------------------------------------------------------------*/

static int wd_cmd_enable(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    ensure_mutex();

    long interval = 10000;
    long deadline = 5000;
    int i;

    for (i = 0; i < argc; i += 2) {
        if (i + 1 >= argc) {
            Jim_SetResultString(interp, "missing value for option", -1);
            return JIM_ERR;
        }
        const char *opt = Jim_String(argv[i]);
        if (strcmp(opt, "-interval") == 0) {
            if (Jim_GetLong(interp, argv[i + 1], &interval) != JIM_OK) return JIM_ERR;
        } else if (strcmp(opt, "-deadline") == 0) {
            if (Jim_GetLong(interp, argv[i + 1], &deadline) != JIM_OK) return JIM_ERR;
        } else {
            Jim_SetResultFormatted(interp, "unknown option \"%s\"", opt);
            return JIM_ERR;
        }
    }

    xSemaphoreTake(wd.mutex, portMAX_DELAY);

    wd.interval_ms = (unsigned long)interval;
    wd.deadline_ms = (unsigned long)deadline;
    wd.main_interp = interp;

    if (wd.timer == NULL) {
        wd.timer = xTimerCreate("watchdog", pdMS_TO_TICKS(interval),
                                pdTRUE, NULL, watchdog_timer_callback);
    } else {
        xTimerChangePeriod(wd.timer, pdMS_TO_TICKS(interval), portMAX_DELAY);
    }

    if (!wd.enabled) {
        xTimerStart(wd.timer, portMAX_DELAY);
        wd.enabled = 1;
        ESP_LOGI(TAG, "Watchdog enabled: interval=%lu ms, deadline=%lu ms",
                 wd.interval_ms, wd.deadline_ms);
    }

    xSemaphoreGive(wd.mutex);
    return JIM_OK;
}

static int wd_cmd_disable(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    ensure_mutex();
    xSemaphoreTake(wd.mutex, portMAX_DELAY);

    if (wd.enabled && wd.timer) {
        xTimerStop(wd.timer, portMAX_DELAY);
        wd.enabled = 0;
        ESP_LOGI(TAG, "Watchdog disabled");
    }

    xSemaphoreGive(wd.mutex);
    return JIM_OK;
}

static int wd_cmd_kick(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    /* Update this VM's last_activity timestamp in the task system */
    Jim_Obj *slot_obj = Jim_GetVariableStr(interp, "task::slot", JIM_NONE);
    if (slot_obj) {
        long slot_idx;
        if (Jim_GetLong(interp, slot_obj, &slot_idx) == JIM_OK &&
            slot_idx >= 0 && slot_idx < TASK_MAX_SLOTS) {
            task_slots[slot_idx].last_activity_us = esp_timer_get_time();
        }
    }
    return JIM_OK;
}

static int wd_cmd_monitor(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    ensure_mutex();

    long task_handle;
    if (Jim_GetLong(interp, argv[0], &task_handle) != JIM_OK) return JIM_ERR;
    if (task_handle < 0 || task_handle >= TASK_MAX_SLOTS || !task_slots[task_handle].in_use) {
        Jim_SetResultFormatted(interp, "invalid task handle: %ld", task_handle);
        return JIM_ERR;
    }

    xSemaphoreTake(wd.mutex, portMAX_DELAY);

    /* Check if already monitored */
    for (int i = 0; i < WD_MAX_MONITORED; i++) {
        if (wd.monitored[i].active && wd.monitored[i].task_slot == (int)task_handle) {
            xSemaphoreGive(wd.mutex);
            return JIM_OK; /* Already monitored */
        }
    }

    /* Find free slot */
    int found = -1;
    for (int i = 0; i < WD_MAX_MONITORED; i++) {
        if (!wd.monitored[i].active) {
            found = i;
            break;
        }
    }

    if (found < 0) {
        xSemaphoreGive(wd.mutex);
        Jim_SetResultString(interp, "maximum monitored tasks reached", -1);
        return JIM_ERR;
    }

    wd_monitored_t *mon = &wd.monitored[found];
    memset(mon, 0, sizeof(*mon));
    mon->active = 1;
    mon->task_slot = (int)task_handle;
    mon->last_pong_us = esp_timer_get_time();

    xSemaphoreGive(wd.mutex);

    ESP_LOGI(TAG, "Now monitoring task '%s' (slot %ld)", task_slots[task_handle].name, task_handle);
    return JIM_OK;
}

static int wd_cmd_unmonitor(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    ensure_mutex();

    long task_handle;
    if (Jim_GetLong(interp, argv[0], &task_handle) != JIM_OK) return JIM_ERR;

    xSemaphoreTake(wd.mutex, portMAX_DELAY);
    for (int i = 0; i < WD_MAX_MONITORED; i++) {
        if (wd.monitored[i].active && wd.monitored[i].task_slot == (int)task_handle) {
            wd.monitored[i].active = 0;
            if (wd.monitored[i].reply_queue) {
                vQueueDelete(wd.monitored[i].reply_queue);
                wd.monitored[i].reply_queue = NULL;
            }
            ESP_LOGI(TAG, "Stopped monitoring task slot %ld", task_handle);
            break;
        }
    }
    xSemaphoreGive(wd.mutex);
    return JIM_OK;
}

static int wd_cmd_policy(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc < 1) {
        Jim_SetResultString(interp, "wrong # args: should be \"watchdog policy task_handle ?opts?\"", -1);
        return JIM_ERR;
    }

    long task_handle;
    if (Jim_GetLong(interp, argv[0], &task_handle) != JIM_OK) return JIM_ERR;
    if (task_handle < 0 || task_handle >= TASK_MAX_SLOTS || !task_slots[task_handle].in_use) {
        Jim_SetResultFormatted(interp, "invalid task handle: %ld", task_handle);
        return JIM_ERR;
    }

    task_slot_t *ts = &task_slots[task_handle];

    /* No additional args: return current policy */
    if (argc == 1) {
        Jim_Obj *result = Jim_NewListObj(interp, NULL, 0);
        Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "max_restarts", -1));
        Jim_ListAppendElement(interp, result, Jim_NewIntObj(interp, ts->cb_max_restarts));
        Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "window_ms", -1));
        Jim_ListAppendElement(interp, result, Jim_NewIntObj(interp, (jim_wide)ts->cb_window_ms));
        Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "cooldown_ms", -1));
        Jim_ListAppendElement(interp, result, Jim_NewIntObj(interp, (jim_wide)ts->cb_cooldown_ms));
        Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "state", -1));
        const char *st;
        switch (ts->cb_state) {
            case CB_CLOSED:    st = "closed"; break;
            case CB_OPEN:      st = "open"; break;
            case CB_HALF_OPEN: st = "half-open"; break;
            default:           st = "unknown";
        }
        Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, st, -1));
        Jim_SetResult(interp, result);
        return JIM_OK;
    }

    /* Set policy options */
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc) {
            Jim_SetResultString(interp, "missing value for option", -1);
            return JIM_ERR;
        }
        const char *opt = Jim_String(argv[i]);
        long val;
        if (Jim_GetLong(interp, argv[i + 1], &val) != JIM_OK) return JIM_ERR;

        if (strcmp(opt, "-max_restarts") == 0) {
            ts->cb_max_restarts = (int)val;
        } else if (strcmp(opt, "-window_ms") == 0) {
            ts->cb_window_ms = (unsigned long)val;
        } else if (strcmp(opt, "-cooldown_ms") == 0) {
            ts->cb_cooldown_ms = (unsigned long)val;
        } else if (strcmp(opt, "-reset") == 0 && val) {
            /* Reset the circuit breaker */
            ts->cb_state = CB_CLOSED;
            memset(ts->restart_timestamps_us, 0, sizeof(ts->restart_timestamps_us));
            ESP_LOGI(TAG, "Circuit breaker reset for task '%s'", ts->name);
        } else {
            Jim_SetResultFormatted(interp, "unknown option \"%s\"", opt);
            return JIM_ERR;
        }
    }

    return JIM_OK;
}

static int wd_cmd_force_sleep(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    const char *mode_str = Jim_String(argv[0]);

    if (strcmp(mode_str, "light") != 0 && strcmp(mode_str, "deep") != 0) {
        Jim_SetResultFormatted(interp, "bad sleep mode \"%s\": should be light or deep", mode_str);
        return JIM_ERR;
    }

    ESP_LOGW(TAG, "FORCED SLEEP requested: mode=%s — bypassing voter consent", mode_str);

    /* Notify critical callback before sleeping */
    if (wd.critical_proc[0] && wd.main_interp) {
        char script[128];
        snprintf(script, sizeof(script), "%s force_sleep system {forced %s sleep}", wd.critical_proc, mode_str);
        Jim_Eval(wd.main_interp, script);
    }

    /* Configure whatever wake sources are registered (from the sleep manager) */
    sleep_manager_t *sm = sleep_manager_get();
    int has_wake = 0;
    for (int i = 0; i < SLEEP_MAX_WAKE_SOURCES; i++) {
        if (sm->wake_sources[i].active) {
            has_wake = 1;
            wake_source_t *ws = &sm->wake_sources[i];
            switch (ws->type) {
                case WAKE_SOURCE_TIMER:
                    esp_sleep_enable_timer_wakeup(ws->config.timer.duration_us);
                    break;
                case WAKE_SOURCE_GPIO:
                    if (strcmp(mode_str, "light") == 0) {
                        esp_sleep_enable_gpio_wakeup();
                    } else {
                        esp_sleep_enable_ext1_wakeup(ws->config.gpio.pin_mask,
                            ws->config.gpio.level ? ESP_EXT1_WAKEUP_ANY_HIGH : ESP_EXT1_WAKEUP_ALL_LOW);
                    }
                    break;
                case WAKE_SOURCE_UART:
                    if (strcmp(mode_str, "light") == 0) {
                        esp_sleep_enable_uart_wakeup(ws->config.uart.uart_num);
                    }
                    break;
                case WAKE_SOURCE_TOUCH:
                    esp_sleep_enable_touchpad_wakeup();
                    break;
            }
        }
    }

    if (!has_wake) {
        /* Emergency fallback: at least set a 60s timer so we don't sleep forever */
        ESP_LOGW(TAG, "No wake sources configured — adding 60s emergency timer");
        esp_sleep_enable_timer_wakeup(60000000ULL);
    }

    if (strcmp(mode_str, "deep") == 0) {
        ESP_LOGW(TAG, "Entering forced DEEP sleep");
        esp_deep_sleep_start();
        /* Never returns */
    } else {
        ESP_LOGW(TAG, "Entering forced LIGHT sleep");
        esp_err_t err = esp_light_sleep_start();
        if (err != ESP_OK) {
            Jim_SetResultFormatted(interp, "forced light sleep failed: %s", esp_err_to_name(err));
            return JIM_ERR;
        }
        esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
        const char *cause_str;
        switch (cause) {
            case ESP_SLEEP_WAKEUP_TIMER:    cause_str = "timer"; break;
            case ESP_SLEEP_WAKEUP_GPIO:     cause_str = "gpio"; break;
            case ESP_SLEEP_WAKEUP_UART:     cause_str = "uart"; break;
            case ESP_SLEEP_WAKEUP_TOUCHPAD: cause_str = "touch"; break;
            default:                        cause_str = "unknown"; break;
        }
        Jim_Obj *result = Jim_NewListObj(interp, NULL, 0);
        Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "status", -1));
        Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "ok", -1));
        Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "wakeup_cause", -1));
        Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, cause_str, -1));
        Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "forced", -1));
        Jim_ListAppendElement(interp, result, Jim_NewIntObj(interp, 1));
        Jim_SetResult(interp, result);
    }

    return JIM_OK;
}

static int wd_cmd_oncritical(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    ensure_mutex();
    const char *proc = Jim_String(argv[0]);

    xSemaphoreTake(wd.mutex, portMAX_DELAY);
    strncpy(wd.critical_proc, proc, sizeof(wd.critical_proc) - 1);
    wd.critical_proc[sizeof(wd.critical_proc) - 1] = '\0';
    wd.main_interp = interp;
    xSemaphoreGive(wd.mutex);

    return JIM_OK;
}

static int wd_cmd_status(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    ensure_mutex();

    Jim_Obj *result = Jim_NewListObj(interp, NULL, 0);

    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "enabled", -1));
    Jim_ListAppendElement(interp, result, Jim_NewIntObj(interp, wd.enabled));

    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "interval_ms", -1));
    Jim_ListAppendElement(interp, result, Jim_NewIntObj(interp, (jim_wide)wd.interval_ms));

    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "deadline_ms", -1));
    Jim_ListAppendElement(interp, result, Jim_NewIntObj(interp, (jim_wide)wd.deadline_ms));

    /* List monitored tasks */
    Jim_Obj *tasks = Jim_NewListObj(interp, NULL, 0);

    xSemaphoreTake(wd.mutex, portMAX_DELAY);
    for (int i = 0; i < WD_MAX_MONITORED; i++) {
        wd_monitored_t *mon = &wd.monitored[i];
        if (!mon->active) continue;

        int slot = mon->task_slot;
        const char *name = (slot >= 0 && slot < TASK_MAX_SLOTS && task_slots[slot].in_use)
                           ? task_slots[slot].name : "?";

        Jim_Obj *entry = Jim_NewListObj(interp, NULL, 0);
        Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, "slot", -1));
        Jim_ListAppendElement(interp, entry, Jim_NewIntObj(interp, slot));
        Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, "name", -1));
        Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, name, -1));
        Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, "failures", -1));
        Jim_ListAppendElement(interp, entry, Jim_NewIntObj(interp, mon->consecutive_failures));
        Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, "wd_restarts", -1));
        Jim_ListAppendElement(interp, entry, Jim_NewIntObj(interp, mon->restarted_by_watchdog));

        int64_t now = esp_timer_get_time();
        int64_t last_seen_ms = (mon->last_pong_us > 0) ? (now - mon->last_pong_us) / 1000 : -1;
        Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, "last_seen_ms", -1));
        Jim_ListAppendElement(interp, entry, Jim_NewIntObj(interp, (jim_wide)last_seen_ms));

        Jim_ListAppendElement(interp, tasks, entry);
    }
    xSemaphoreGive(wd.mutex);

    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "monitored", -1));
    Jim_ListAppendElement(interp, result, tasks);

    Jim_SetResult(interp, result);
    return JIM_OK;
}

static const jim_subcmd_type watchdog_command_table[] = {
    {   "enable",
        "?-interval ms? ?-deadline ms?",
        wd_cmd_enable,
        0,
        -1,
        /* Description: Start the watchdog monitor */
    },
    {   "disable",
        NULL,
        wd_cmd_disable,
        0,
        0,
        /* Description: Stop the watchdog monitor */
    },
    {   "kick",
        NULL,
        wd_cmd_kick,
        0,
        0,
        /* Description: Signal liveness from the current VM */
    },
    {   "monitor",
        "task_handle",
        wd_cmd_monitor,
        1,
        1,
        /* Description: Add a task to watchdog monitoring */
    },
    {   "unmonitor",
        "task_handle",
        wd_cmd_unmonitor,
        1,
        1,
        /* Description: Remove a task from watchdog monitoring */
    },
    {   "policy",
        "task_handle ?-max_restarts n? ?-window_ms ms? ?-cooldown_ms ms? ?-reset 1?",
        wd_cmd_policy,
        1,
        -1,
        /* Description: Get or set circuit breaker policy for a task */
    },
    {   "force_sleep",
        "light|deep",
        wd_cmd_force_sleep,
        1,
        1,
        /* Description: Force sleep immediately, bypassing voter consent */
    },
    {   "oncritical",
        "proc",
        wd_cmd_oncritical,
        1,
        1,
        /* Description: Register callback for critical events */
    },
    {   "status",
        NULL,
        wd_cmd_status,
        0,
        0,
        /* Description: Show watchdog state and monitored task health */
    },
    { NULL }
};

int Jim_watchdogInit(Jim_Interp *interp)
{
    Jim_PackageProvideCheck(interp, "watchdog");
    Jim_RegisterSubCmd(interp, "watchdog", watchdog_command_table, NULL);
    return JIM_OK;
}
