/* Jim Tcl ESP32 Sleep Extension
 *
 * Coordinated power management across multiple Tcl VMs. Any VM can propose
 * that the system enter a low-power state. All registered VMs are consulted
 * and must unanimously approve before sleep proceeds. VMs can also register
 * wake sources (GPIO, timer, UART, touch) that are configured before sleep.
 *
 * Protocol:
 *   1. VM calls: sleep request light|deep
 *   2. Sleep manager iterates all registered voters
 *   3. Each voter's callback proc is evaluated in its interpreter
 *      - Proc receives the proposed mode as an argument
 *      - Returns "ok" to approve, or anything else to veto
 *   4. If unanimous approval: wake sources are configured and system sleeps
 *   5. If any veto: sleep is cancelled, requester gets a report of who vetoed
 *
 * Commands:
 *
 *   sleep register <callback_proc>
 *       Register this VM as a sleep voter. The callback proc will be called
 *       with one argument (the sleep mode) when sleep is proposed. It should
 *       return "ok" to approve or a reason string to veto.
 *
 *   sleep unregister
 *       Remove this VM from sleep voting.
 *
 *   sleep request light|deep ?-timeout ms?
 *       Propose system sleep. Consults all voters. Returns "ok" if sleep
 *       happened, or a dict of vetoes if blocked. Timeout controls max
 *       wait for voter responses (default 5000 ms).
 *
 *   sleep wake timer <microseconds>
 *       Register a timer wake source.
 *
 *   sleep wake gpio <pin_mask> ?high|low?
 *       Register GPIO wake source. pin_mask is a bitmask of GPIO pins.
 *       Default trigger level is low.
 *
 *   sleep wake uart ?uart_num?
 *       Register UART wake source (light sleep only). Default UART 0.
 *
 *   sleep wake touch <pad_num>
 *       Register touch pad wake source.
 *
 *   sleep wake clear
 *       Remove all wake sources registered by this VM.
 *
 *   sleep wake list
 *       List all registered wake sources (from all VMs).
 *
 *   sleep voters
 *       List all registered sleep voters.
 *
 *   sleep status
 *       Return sleep manager state: idle or pending.
 */

#include <string.h>
#include <stdlib.h>
#include "jim.h"
#include "jim-subcmd.h"
#include "jim-esp32-sleep.h"
#include "jim-esp32-task.h"
#include "esp_sleep.h"
#include "soc/soc_caps.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "jim-sleep";

/* ---------------------------------------------------------------------------
 * Sleep manager singleton
 * ---------------------------------------------------------------------------*/

static sleep_manager_t manager = { 0 };

sleep_manager_t *sleep_manager_get(void)
{
    return &manager;
}

void sleep_manager_init(void)
{
    if (manager.mutex == NULL) {
        manager.mutex = xSemaphoreCreateMutex();
    }
}

int sleep_manager_register(const char *name, QueueHandle_t msg_queue,
                           const char *callback_proc, int is_main)
{
    sleep_manager_init();
    xSemaphoreTake(manager.mutex, portMAX_DELAY);

    int id = -1;
    for (int i = 0; i < SLEEP_MAX_VOTERS; i++) {
        if (!manager.voters[i].active) {
            sleep_voter_t *v = &manager.voters[i];
            v->active = 1;
            strncpy(v->name, name, sizeof(v->name) - 1);
            v->name[sizeof(v->name) - 1] = '\0';
            v->msg_queue = msg_queue;
            v->is_main = is_main;
            strncpy(v->callback_proc, callback_proc, sizeof(v->callback_proc) - 1);
            v->callback_proc[sizeof(v->callback_proc) - 1] = '\0';
            v->last_vote = SLEEP_VOTE_PENDING;
            v->veto_reason[0] = '\0';
            if (v->vote_reply == NULL) {
                v->vote_reply = xQueueCreate(1, sizeof(sleep_vote_t));
            }
            id = i;
            ESP_LOGI(TAG, "Voter '%s' registered (id=%d, callback=%s)", name, id, callback_proc);
            break;
        }
    }

    xSemaphoreGive(manager.mutex);
    return id;
}

void sleep_manager_unregister(int voter_id)
{
    if (voter_id < 0 || voter_id >= SLEEP_MAX_VOTERS) return;
    sleep_manager_init();
    xSemaphoreTake(manager.mutex, portMAX_DELAY);

    sleep_voter_t *v = &manager.voters[voter_id];
    if (v->active) {
        /* Also remove any wake sources this voter registered */
        for (int i = 0; i < SLEEP_MAX_WAKE_SOURCES; i++) {
            if (manager.wake_sources[i].active && manager.wake_sources[i].voter_id == voter_id) {
                manager.wake_sources[i].active = 0;
            }
        }
        ESP_LOGI(TAG, "Voter '%s' unregistered (id=%d)", v->name, voter_id);
        v->active = 0;
        v->callback_proc[0] = '\0';
    }

    xSemaphoreGive(manager.mutex);
}

void sleep_manager_set_callback(int voter_id, const char *callback_proc)
{
    if (voter_id < 0 || voter_id >= SLEEP_MAX_VOTERS) return;
    sleep_manager_init();
    xSemaphoreTake(manager.mutex, portMAX_DELAY);

    sleep_voter_t *v = &manager.voters[voter_id];
    if (v->active) {
        strncpy(v->callback_proc, callback_proc, sizeof(v->callback_proc) - 1);
        v->callback_proc[sizeof(v->callback_proc) - 1] = '\0';
    }

    xSemaphoreGive(manager.mutex);
}

int sleep_manager_add_wake_source(int voter_id, wake_source_type_t type)
{
    sleep_manager_init();
    xSemaphoreTake(manager.mutex, portMAX_DELAY);

    int id = -1;
    for (int i = 0; i < SLEEP_MAX_WAKE_SOURCES; i++) {
        if (!manager.wake_sources[i].active) {
            manager.wake_sources[i].active = 1;
            manager.wake_sources[i].type = type;
            manager.wake_sources[i].voter_id = voter_id;
            memset(&manager.wake_sources[i].config, 0, sizeof(manager.wake_sources[i].config));
            id = i;
            break;
        }
    }

    xSemaphoreGive(manager.mutex);
    return id;
}

void sleep_manager_remove_wake_source(int source_id)
{
    if (source_id < 0 || source_id >= SLEEP_MAX_WAKE_SOURCES) return;
    sleep_manager_init();
    xSemaphoreTake(manager.mutex, portMAX_DELAY);
    manager.wake_sources[source_id].active = 0;
    xSemaphoreGive(manager.mutex);
}

wake_source_t *sleep_manager_get_wake_source(int source_id)
{
    if (source_id < 0 || source_id >= SLEEP_MAX_WAKE_SOURCES) return NULL;
    return &manager.wake_sources[source_id];
}

/* ---------------------------------------------------------------------------
 * Per-interpreter state: tracks this VM's voter_id
 * ---------------------------------------------------------------------------*/

#define SLEEP_ASSOC_KEY "jim-sleep-voter-id"

typedef struct {
    int voter_id;
} sleep_interp_data_t;

static void sleep_interp_free(Jim_Interp *interp, void *data)
{
    sleep_interp_data_t *sid = (sleep_interp_data_t *)data;
    if (sid->voter_id >= 0) {
        sleep_manager_unregister(sid->voter_id);
    }
    Jim_Free(sid);
}

static sleep_interp_data_t *get_sleep_data(Jim_Interp *interp)
{
    sleep_interp_data_t *sid = Jim_GetAssocData(interp, SLEEP_ASSOC_KEY);
    if (!sid) {
        sid = Jim_Alloc(sizeof(*sid));
        sid->voter_id = -1;
        Jim_SetAssocData(interp, SLEEP_ASSOC_KEY, sleep_interp_free, sid);
    }
    return sid;
}

/* ---------------------------------------------------------------------------
 * Consultation: evaluate a voter's callback and collect the vote
 *
 * For the main interpreter (is_main=1), we evaluate directly.
 * For task VMs, we send a message through the task's queue and wait
 * for the reply on the voter's dedicated vote_reply queue.
 * ---------------------------------------------------------------------------*/

/* Message type added to the task message protocol for sleep consultation.
 * We reuse the task system's eval mechanism by sending a specially crafted
 * script that calls the callback and posts the result. */

static int consult_voter_main(Jim_Interp *interp, sleep_voter_t *voter, const char *mode_str)
{
    /* Direct eval in the calling interpreter (this IS the main VM) */
    char script[128];
    snprintf(script, sizeof(script), "%s %s", voter->callback_proc, mode_str);

    int ret = Jim_Eval(interp, script);
    const char *result = Jim_String(Jim_GetResult(interp));
    if (ret == JIM_OK && strcmp(result, "ok") == 0) {
        voter->last_vote = SLEEP_VOTE_APPROVE;
        voter->veto_reason[0] = '\0';
    } else {
        voter->last_vote = SLEEP_VOTE_VETO;
        strncpy(voter->veto_reason, result, sizeof(voter->veto_reason) - 1);
        voter->veto_reason[sizeof(voter->veto_reason) - 1] = '\0';
    }
    return (voter->last_vote == SLEEP_VOTE_APPROVE) ? 0 : 1;
}

/* For task VMs: we build a script that calls the callback, then
 * use the task's existing msg_queue with TASK_MSG_EVAL to get the result.
 * We repurpose the task_msg_t/task_reply_t protocol from jim-esp-task.c. */

static int consult_voter_task(sleep_voter_t *voter, const char *mode_str, long timeout_ms)
{
    if (!voter->msg_queue) {
        voter->last_vote = SLEEP_VOTE_VETO;
        strncpy(voter->veto_reason, "no message queue", sizeof(voter->veto_reason) - 1);
        voter->veto_reason[sizeof(voter->veto_reason) - 1] = '\0';
        return 1;
    }

    /* Build the consultation script */
    char script[128];
    snprintf(script, sizeof(script), "%s %s", voter->callback_proc, mode_str);

    /* Create a temporary reply queue for this consultation */
    QueueHandle_t reply_q = xQueueCreate(1, sizeof(task_reply_t));
    if (!reply_q) {
        voter->last_vote = SLEEP_VOTE_VETO;
        return 1;
    }

    task_msg_t msg;
    msg.type = TASK_MSG_EVAL;
    msg.script = strdup(script);
    msg.reply_queue = reply_q;

    if (xQueueSend(voter->msg_queue, &msg, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        free(msg.script);
        vQueueDelete(reply_q);
        voter->last_vote = SLEEP_VOTE_VETO;
        strncpy(voter->veto_reason, "queue full", sizeof(voter->veto_reason) - 1);
        voter->veto_reason[sizeof(voter->veto_reason) - 1] = '\0';
        ESP_LOGW(TAG, "Voter '%s': queue full, counted as veto", voter->name);
        return 1;
    }

    /* Wait for response.
     * On timeout, do NOT delete reply_q — the task may still write to it. */
    task_reply_t reply;
    if (xQueueReceive(reply_q, &reply, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        voter->last_vote = SLEEP_VOTE_VETO;
        strncpy(voter->veto_reason, "timeout", sizeof(voter->veto_reason) - 1);
        voter->veto_reason[sizeof(voter->veto_reason) - 1] = '\0';
        ESP_LOGW(TAG, "Voter '%s': timeout, counted as veto", voter->name);
        return 1;
    }

    vQueueDelete(reply_q);

    if (reply.retcode == JIM_OK && reply.result && strcmp(reply.result, "ok") == 0) {
        voter->last_vote = SLEEP_VOTE_APPROVE;
        voter->veto_reason[0] = '\0';
    } else {
        voter->last_vote = SLEEP_VOTE_VETO;
        if (reply.result) {
            strncpy(voter->veto_reason, reply.result, sizeof(voter->veto_reason) - 1);
            voter->veto_reason[sizeof(voter->veto_reason) - 1] = '\0';
        } else {
            strncpy(voter->veto_reason, "no response", sizeof(voter->veto_reason) - 1);
        }
    }

    if (reply.result) free(reply.result);
    return (voter->last_vote == SLEEP_VOTE_APPROVE) ? 0 : 1;
}

/* ---------------------------------------------------------------------------
 * Configure ESP-IDF wake sources before sleep
 * ---------------------------------------------------------------------------*/

static int configure_wake_sources(Jim_Interp *interp, sleep_mode_t mode)
{
    int source_count = 0;

    for (int i = 0; i < SLEEP_MAX_WAKE_SOURCES; i++) {
        wake_source_t *ws = &manager.wake_sources[i];
        if (!ws->active) continue;

        esp_err_t err = ESP_OK;
        switch (ws->type) {
            case WAKE_SOURCE_TIMER:
                err = esp_sleep_enable_timer_wakeup(ws->config.timer.duration_us);
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "Wake source: timer %llu us", ws->config.timer.duration_us);
                }
                break;

            case WAKE_SOURCE_GPIO:
                if (mode == SLEEP_MODE_LIGHT) {
                    /* Configure each pin in the mask for gpio wakeup */
                    for (int pin = 0; pin < 64; pin++) {
                        if (ws->config.gpio.pin_mask & (1ULL << pin)) {
                            err = gpio_wakeup_enable((gpio_num_t)pin,
                                ws->config.gpio.level ? GPIO_INTR_HIGH_LEVEL : GPIO_INTR_LOW_LEVEL);
                            if (err != ESP_OK) {
                                ESP_LOGE(TAG, "gpio_wakeup_enable failed for GPIO %d: %s",
                                         pin, esp_err_to_name(err));
                                break;
                            }
                        }
                    }
                    if (err == ESP_OK) {
                        err = esp_sleep_enable_gpio_wakeup();
                    }
                } else {
                    err = esp_sleep_enable_ext1_wakeup(ws->config.gpio.pin_mask,
                        ws->config.gpio.level ? ESP_EXT1_WAKEUP_ANY_HIGH : ESP_EXT1_WAKEUP_ALL_LOW);
                }
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "Wake source: GPIO mask=0x%llx level=%d",
                             ws->config.gpio.pin_mask, ws->config.gpio.level);
                }
                break;

            case WAKE_SOURCE_UART:
                if (mode == SLEEP_MODE_LIGHT) {
                    err = esp_sleep_enable_uart_wakeup(ws->config.uart.uart_num);
                    if (err == ESP_OK) {
                        ESP_LOGI(TAG, "Wake source: UART %d", ws->config.uart.uart_num);
                    }
                } else {
                    ESP_LOGW(TAG, "UART wake not supported in deep sleep, skipping");
                    continue;
                }
                break;

            case WAKE_SOURCE_TOUCH:
#if SOC_PM_SUPPORT_TOUCH_SENSOR_WAKEUP
                err = esp_sleep_enable_touchpad_wakeup();
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "Wake source: touch pad %d", ws->config.touch.touch_pad);
                }
#else
                ESP_LOGW(TAG, "Touch wakeup not supported on this chip, skipping");
                continue;
#endif
                break;
        }

        if (err != ESP_OK) {
            Jim_SetResultFormatted(interp, "failed to configure wake source %d: %s",
                                   i, esp_err_to_name(err));
            return JIM_ERR;
        }
        source_count++;
    }

    if (source_count == 0) {
        Jim_SetResultString(interp, "no wake sources configured - refusing to sleep without a way to wake", -1);
        return JIM_ERR;
    }

    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Tcl commands
 * ---------------------------------------------------------------------------*/

static int sleep_cmd_register(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    sleep_manager_init();

    const char *callback = Jim_String(argv[0]);
    sleep_interp_data_t *sid = get_sleep_data(interp);

    if (sid->voter_id >= 0) {
        /* Already registered - just update the callback */
        sleep_manager_set_callback(sid->voter_id, callback);
        Jim_SetResultInt(interp, sid->voter_id);
        return JIM_OK;
    }

    /* Determine our name and queue */
    const char *name = "main";
    QueueHandle_t msg_queue = NULL;
    int is_main = 1;

    Jim_Obj *name_obj = Jim_GetVariableStr(interp, "task::self", JIM_NONE);
    if (name_obj) {
        name = Jim_String(name_obj);
        is_main = 0;
    }

    /* For task VMs, we need the task's message queue. The task slot index
     * is stored in task::slot variable. We access the task system's slot
     * array via an extern. */
    Jim_Obj *slot_obj = Jim_GetVariableStr(interp, "task::slot", JIM_NONE);
    if (slot_obj) {
        /* The task extension stores the msg_queue in its slot structure.
         * We retrieve it by sending ourselves a message via the sleep
         * registration mechanism - the queue pointer is set by the
         * caller who knows it. For now, we use an alternate approach:
         * store the queue handle in an interp assoc data during task init. */
        void *queue_ptr = Jim_GetAssocData(interp, "task.msg_queue");
        if (queue_ptr) {
            msg_queue = (QueueHandle_t)queue_ptr;
        }
    }

    int voter_id = sleep_manager_register(name, msg_queue, callback, is_main);
    if (voter_id < 0) {
        Jim_SetResultString(interp, "maximum number of sleep voters reached", -1);
        return JIM_ERR;
    }

    sid->voter_id = voter_id;
    Jim_SetResultInt(interp, voter_id);
    return JIM_OK;
}

static int sleep_cmd_unregister(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    sleep_interp_data_t *sid = get_sleep_data(interp);
    if (sid->voter_id >= 0) {
        sleep_manager_unregister(sid->voter_id);
        sid->voter_id = -1;
    }
    return JIM_OK;
}

static int sleep_cmd_request(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    sleep_manager_init();

    const char *mode_str = Jim_String(argv[0]);
    sleep_mode_t mode;

    if (strcmp(mode_str, "light") == 0) {
        mode = SLEEP_MODE_LIGHT;
    } else if (strcmp(mode_str, "deep") == 0) {
        mode = SLEEP_MODE_DEEP;
    } else {
        Jim_SetResultFormatted(interp, "bad sleep mode \"%s\": should be light or deep", mode_str);
        return JIM_ERR;
    }

    long timeout_ms = 5000;
    int force = 0;
    int i;
    for (i = 1; i < argc; i++) {
        const char *opt = Jim_String(argv[i]);
        if (strcmp(opt, "-force") == 0) {
            force = 1;
        } else if (strcmp(opt, "-timeout") == 0) {
            if (i + 1 >= argc) {
                Jim_SetResultString(interp, "missing value for -timeout", -1);
                return JIM_ERR;
            }
            i++;
            if (Jim_GetLong(interp, argv[i], &timeout_ms) != JIM_OK) return JIM_ERR;
        } else {
            Jim_SetResultFormatted(interp, "unknown option \"%s\"", opt);
            return JIM_ERR;
        }
    }

    if (force) {
        ESP_LOGW(TAG, "FORCED sleep requested: mode=%s — skipping voter consultation", mode_str);
        goto do_sleep;
    }

    ESP_LOGI(TAG, "Sleep requested: mode=%s, consulting voters...", mode_str);

    xSemaphoreTake(manager.mutex, portMAX_DELAY);
    manager.sleep_pending = 1;
    manager.pending_mode = mode;
    manager.vote_count = 0;
    manager.veto_count = 0;

    /* Count active voters */
    int total_voters = 0;
    for (i = 0; i < SLEEP_MAX_VOTERS; i++) {
        if (manager.voters[i].active) total_voters++;
    }

    xSemaphoreGive(manager.mutex);

    if (total_voters == 0) {
        ESP_LOGI(TAG, "No voters registered, proceeding with sleep");
        goto do_sleep;
    }

    /* Consult each voter */
    Jim_Obj *vetoes = Jim_NewListObj(interp, NULL, 0);
    int veto_count = 0;

    xSemaphoreTake(manager.mutex, portMAX_DELAY);
    for (i = 0; i < SLEEP_MAX_VOTERS; i++) {
        sleep_voter_t *voter = &manager.voters[i];
        if (!voter->active) continue;
        if (voter->callback_proc[0] == '\0') {
            /* No callback = implicit approve */
            voter->last_vote = SLEEP_VOTE_APPROVE;
            continue;
        }

        int vetoed;
        if (voter->is_main) {
            /* Main interpreter - evaluate directly (we're in it) */
            xSemaphoreGive(manager.mutex);
            vetoed = consult_voter_main(interp, voter, mode_str);
            xSemaphoreTake(manager.mutex, portMAX_DELAY);
        } else {
            /* Task VM - send message and wait */
            xSemaphoreGive(manager.mutex);
            vetoed = consult_voter_task(voter, mode_str, timeout_ms);
            xSemaphoreTake(manager.mutex, portMAX_DELAY);
        }

        if (vetoed) {
            veto_count++;
            /* Add veto info to result */
            Jim_Obj *veto_entry = Jim_NewListObj(interp, NULL, 0);
            Jim_ListAppendElement(interp, veto_entry, Jim_NewStringObj(interp, "voter", -1));
            Jim_ListAppendElement(interp, veto_entry, Jim_NewStringObj(interp, voter->name, -1));
            Jim_ListAppendElement(interp, veto_entry, Jim_NewStringObj(interp, "reason", -1));
            const char *reason = voter->veto_reason[0] ? voter->veto_reason : "unspecified";
            Jim_ListAppendElement(interp, veto_entry, Jim_NewStringObj(interp, reason, -1));
            Jim_ListAppendElement(interp, vetoes, veto_entry);
            ESP_LOGW(TAG, "Voter '%s' vetoed sleep", voter->name);
        } else {
            ESP_LOGI(TAG, "Voter '%s' approved sleep", voter->name);
        }
    }

    manager.sleep_pending = 0;
    xSemaphoreGive(manager.mutex);

    if (veto_count > 0) {
        /* Sleep blocked */
        Jim_Obj *result = Jim_NewListObj(interp, NULL, 0);
        Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "status", -1));
        Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "vetoed", -1));
        Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "vetoes", -1));
        Jim_ListAppendElement(interp, result, vetoes);
        Jim_SetResult(interp, result);
        return JIM_OK;
    }

do_sleep:
    ESP_LOGI(TAG, "All voters approved. Configuring wake sources...");

    /* Configure all registered wake sources */
    if (configure_wake_sources(interp, mode) != JIM_OK) {
        return JIM_ERR;
    }

    ESP_LOGI(TAG, "Entering %s sleep", mode_str);

    if (mode == SLEEP_MODE_LIGHT) {
        esp_err_t err = esp_light_sleep_start();
        if (err != ESP_OK) {
            Jim_SetResultFormatted(interp, "light sleep failed: %s", esp_err_to_name(err));
            return JIM_ERR;
        }

        /* We woke up - report the cause */
        esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
        const char *cause_str;
        switch (cause) {
            case ESP_SLEEP_WAKEUP_TIMER:     cause_str = "timer"; break;
            case ESP_SLEEP_WAKEUP_GPIO:      cause_str = "gpio"; break;
            case ESP_SLEEP_WAKEUP_UART:      cause_str = "uart"; break;
            case ESP_SLEEP_WAKEUP_TOUCHPAD:  cause_str = "touch"; break;
            case ESP_SLEEP_WAKEUP_EXT1:      cause_str = "ext1"; break;
            default:                         cause_str = "unknown"; break;
        }

        Jim_Obj *result = Jim_NewListObj(interp, NULL, 0);
        Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "status", -1));
        Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "ok", -1));
        Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "wakeup_cause", -1));
        Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, cause_str, -1));
        Jim_SetResult(interp, result);
        ESP_LOGI(TAG, "Woke from light sleep: cause=%s", cause_str);
    }
    else {
        /* Deep sleep - does not return */
        ESP_LOGI(TAG, "Entering deep sleep (will not return)...");
        esp_deep_sleep_start();
        /* Never reached */
    }

    return JIM_OK;
}

static int sleep_cmd_wake(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    sleep_manager_init();
    sleep_interp_data_t *sid = get_sleep_data(interp);
    int voter_id = sid->voter_id;

    const char *subcmd = Jim_String(argv[0]);

    if (strcmp(subcmd, "clear") == 0) {
        /* Remove all wake sources from this voter */
        if (voter_id >= 0) {
            xSemaphoreTake(manager.mutex, portMAX_DELAY);
            for (int i = 0; i < SLEEP_MAX_WAKE_SOURCES; i++) {
                if (manager.wake_sources[i].active && manager.wake_sources[i].voter_id == voter_id) {
                    manager.wake_sources[i].active = 0;
                }
            }
            xSemaphoreGive(manager.mutex);
        }
        return JIM_OK;
    }

    if (strcmp(subcmd, "list") == 0) {
        Jim_Obj *result = Jim_NewListObj(interp, NULL, 0);
        xSemaphoreTake(manager.mutex, portMAX_DELAY);
        for (int i = 0; i < SLEEP_MAX_WAKE_SOURCES; i++) {
            wake_source_t *ws = &manager.wake_sources[i];
            if (!ws->active) continue;

            Jim_Obj *entry = Jim_NewListObj(interp, NULL, 0);
            Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, "id", -1));
            Jim_ListAppendElement(interp, entry, Jim_NewIntObj(interp, i));
            Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, "type", -1));

            const char *type_str;
            switch (ws->type) {
                case WAKE_SOURCE_TIMER: type_str = "timer"; break;
                case WAKE_SOURCE_GPIO:  type_str = "gpio"; break;
                case WAKE_SOURCE_UART:  type_str = "uart"; break;
                case WAKE_SOURCE_TOUCH: type_str = "touch"; break;
                default: type_str = "unknown";
            }
            Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, type_str, -1));

            Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, "voter", -1));
            /* Find voter name */
            const char *vname = "unknown";
            if (ws->voter_id >= 0 && ws->voter_id < SLEEP_MAX_VOTERS && manager.voters[ws->voter_id].active) {
                vname = manager.voters[ws->voter_id].name;
            }
            Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, vname, -1));

            Jim_ListAppendElement(interp, result, entry);
        }
        xSemaphoreGive(manager.mutex);
        Jim_SetResult(interp, result);
        return JIM_OK;
    }

    /* All wake source types below require voter registration */
    if (voter_id < 0) {
        Jim_SetResultString(interp, "must call 'sleep register' before adding wake sources", -1);
        return JIM_ERR;
    }

    if (strcmp(subcmd, "timer") == 0) {
        if (argc < 2) {
            Jim_SetResultString(interp, "wrong # args: should be \"sleep wake timer microseconds\"", -1);
            return JIM_ERR;
        }
        long long duration_us;
        jim_wide wide_val;
        if (Jim_GetWide(interp, argv[1], &wide_val) != JIM_OK) return JIM_ERR;
        duration_us = (long long)wide_val;

        if (duration_us <= 0) {
            Jim_SetResultString(interp, "timer duration must be positive", -1);
            return JIM_ERR;
        }

        int src_id = sleep_manager_add_wake_source(voter_id, WAKE_SOURCE_TIMER);
        if (src_id < 0) {
            Jim_SetResultString(interp, "maximum wake sources reached", -1);
            return JIM_ERR;
        }
        xSemaphoreTake(manager.mutex, portMAX_DELAY);
        manager.wake_sources[src_id].config.timer.duration_us = (uint64_t)duration_us;
        xSemaphoreGive(manager.mutex);
        Jim_SetResultInt(interp, src_id);
        return JIM_OK;
    }

    if (strcmp(subcmd, "gpio") == 0) {
        if (argc < 2) {
            Jim_SetResultString(interp, "wrong # args: should be \"sleep wake gpio pin_mask ?high|low?\"", -1);
            return JIM_ERR;
        }
        jim_wide mask_val;
        if (Jim_GetWide(interp, argv[1], &mask_val) != JIM_OK) return JIM_ERR;

        int level = 0; /* default: wake on low */
        if (argc >= 3) {
            const char *level_str = Jim_String(argv[2]);
            if (strcmp(level_str, "high") == 0) {
                level = 1;
            } else if (strcmp(level_str, "low") == 0) {
                level = 0;
            } else {
                Jim_SetResultFormatted(interp, "bad level \"%s\": should be high or low", level_str);
                return JIM_ERR;
            }
        }

        int src_id = sleep_manager_add_wake_source(voter_id, WAKE_SOURCE_GPIO);
        if (src_id < 0) {
            Jim_SetResultString(interp, "maximum wake sources reached", -1);
            return JIM_ERR;
        }
        xSemaphoreTake(manager.mutex, portMAX_DELAY);
        manager.wake_sources[src_id].config.gpio.pin_mask = (uint64_t)mask_val;
        manager.wake_sources[src_id].config.gpio.level = level;
        xSemaphoreGive(manager.mutex);
        Jim_SetResultInt(interp, src_id);
        return JIM_OK;
    }

    if (strcmp(subcmd, "uart") == 0) {
        long uart_num = 0;
        if (argc >= 2) {
            if (Jim_GetLong(interp, argv[1], &uart_num) != JIM_OK) return JIM_ERR;
        }

        int src_id = sleep_manager_add_wake_source(voter_id, WAKE_SOURCE_UART);
        if (src_id < 0) {
            Jim_SetResultString(interp, "maximum wake sources reached", -1);
            return JIM_ERR;
        }
        xSemaphoreTake(manager.mutex, portMAX_DELAY);
        manager.wake_sources[src_id].config.uart.uart_num = (int)uart_num;
        xSemaphoreGive(manager.mutex);
        Jim_SetResultInt(interp, src_id);
        return JIM_OK;
    }

    if (strcmp(subcmd, "touch") == 0) {
        if (argc < 2) {
            Jim_SetResultString(interp, "wrong # args: should be \"sleep wake touch pad_num\"", -1);
            return JIM_ERR;
        }
        long pad;
        if (Jim_GetLong(interp, argv[1], &pad) != JIM_OK) return JIM_ERR;

        int src_id = sleep_manager_add_wake_source(voter_id, WAKE_SOURCE_TOUCH);
        if (src_id < 0) {
            Jim_SetResultString(interp, "maximum wake sources reached", -1);
            return JIM_ERR;
        }
        xSemaphoreTake(manager.mutex, portMAX_DELAY);
        manager.wake_sources[src_id].config.touch.touch_pad = (int)pad;
        xSemaphoreGive(manager.mutex);
        Jim_SetResultInt(interp, src_id);
        return JIM_OK;
    }

    Jim_SetResultFormatted(interp,
        "unknown wake subcommand \"%s\": should be timer, gpio, uart, touch, clear, or list", subcmd);
    return JIM_ERR;
}

static int sleep_cmd_voters(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    sleep_manager_init();

    Jim_Obj *result = Jim_NewListObj(interp, NULL, 0);

    xSemaphoreTake(manager.mutex, portMAX_DELAY);
    for (int i = 0; i < SLEEP_MAX_VOTERS; i++) {
        sleep_voter_t *v = &manager.voters[i];
        if (!v->active) continue;

        Jim_Obj *entry = Jim_NewListObj(interp, NULL, 0);
        Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, "id", -1));
        Jim_ListAppendElement(interp, entry, Jim_NewIntObj(interp, i));
        Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, "name", -1));
        Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, v->name, -1));
        Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, "callback", -1));
        Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp,
            v->callback_proc[0] ? v->callback_proc : "(none)", -1));
        Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, "type", -1));
        Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp,
            v->is_main ? "main" : "task", -1));
        Jim_ListAppendElement(interp, result, entry);
    }
    xSemaphoreGive(manager.mutex);

    Jim_SetResult(interp, result);
    return JIM_OK;
}

static int sleep_cmd_status(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    sleep_manager_init();

    xSemaphoreTake(manager.mutex, portMAX_DELAY);
    const char *status = manager.sleep_pending ? "pending" : "idle";
    xSemaphoreGive(manager.mutex);

    Jim_SetResultString(interp, status, -1);
    return JIM_OK;
}

static const jim_subcmd_type sleep_command_table[] = {
    {   "register",
        "callback_proc",
        sleep_cmd_register,
        1,
        1,
        /* Description: Register this VM as a sleep voter */
    },
    {   "unregister",
        NULL,
        sleep_cmd_unregister,
        0,
        0,
        /* Description: Remove this VM from sleep voting */
    },
    {   "request",
        "light|deep ?-timeout ms?",
        sleep_cmd_request,
        1,
        -1,
        /* Description: Propose system sleep (consults all voters) */
    },
    {   "wake",
        "timer|gpio|uart|touch|clear|list ...",
        sleep_cmd_wake,
        1,
        -1,
        /* Description: Manage wake sources */
    },
    {   "voters",
        NULL,
        sleep_cmd_voters,
        0,
        0,
        /* Description: List all registered sleep voters */
    },
    {   "status",
        NULL,
        sleep_cmd_status,
        0,
        0,
        /* Description: Return sleep manager state (idle or pending) */
    },
    { NULL }
};

int Jim_sleepInit(Jim_Interp *interp)
{
    Jim_PackageProvideCheck(interp, "sleep");
    Jim_RegisterSubCmd(interp, "sleep", sleep_command_table, NULL);
    return JIM_OK;
}
