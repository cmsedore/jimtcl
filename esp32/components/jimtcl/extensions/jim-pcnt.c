/* Jim Tcl PCNT Extension for ESP32
 *
 * Provides Tcl commands for the Pulse Counter peripheral:
 *
 *   pcnt init <gpio> ?-edge rising|falling|both? ?-ctrl_gpio pin? ?-high_limit n? ?-low_limit n?
 *   pcnt read <handle>
 *   pcnt clear <handle>
 *   pcnt pause <handle>
 *   pcnt resume <handle>
 *   pcnt watch <handle> <threshold> -callback {proc task}
 *   pcnt deinit <handle>
 */

#include <string.h>
#include <stdlib.h>
#include "jim.h"
#include "jim-subcmd.h"
#include "driver/pulse_cnt.h"
#include "esp_log.h"
#include "jim-esp32-task.h"

static const char *TAG = "jim-pcnt";

#define PCNT_MAX_UNITS 4
#define PCNT_MAX_WATCHES 4  /* max watch points per unit */

typedef struct {
    int active;
    int threshold;
    char callback_proc[64];
    char callback_task[16];
} pcnt_watch_t;

typedef struct {
    int in_use;
    int gpio;
    pcnt_unit_handle_t unit;
    pcnt_channel_handle_t channel;
    int high_limit;
    int low_limit;
    int paused;
    pcnt_watch_t watches[PCNT_MAX_WATCHES];
} pcnt_state_t;

static pcnt_state_t pcnt_units[PCNT_MAX_UNITS] = { 0 };

static int pcnt_find_free(void)
{
    for (int i = 0; i < PCNT_MAX_UNITS; i++) {
        if (!pcnt_units[i].in_use) return i;
    }
    return -1;
}

/* Watch point callback - runs in ISR context, posts to task */
static bool IRAM_ATTR pcnt_watch_callback(pcnt_unit_handle_t unit,
                                           const pcnt_watch_event_data_t *edata,
                                           void *user_ctx)
{
    pcnt_state_t *state = (pcnt_state_t *)user_ctx;
    int watch_point = edata->watch_point_value;

    for (int i = 0; i < PCNT_MAX_WATCHES; i++) {
        if (state->watches[i].active && state->watches[i].threshold == watch_point) {
            char script[128];
            snprintf(script, sizeof(script), "%s %d",
                     state->watches[i].callback_proc, watch_point);
            task_send_to_name(state->watches[i].callback_task, script);
            break;
        }
    }
    return false;
}

static int parse_edge(Jim_Interp *interp, const char *str,
                      pcnt_channel_edge_action_t *rising_action,
                      pcnt_channel_edge_action_t *falling_action)
{
    if (strcmp(str, "rising") == 0) {
        *rising_action = PCNT_CHANNEL_EDGE_ACTION_INCREASE;
        *falling_action = PCNT_CHANNEL_EDGE_ACTION_HOLD;
    } else if (strcmp(str, "falling") == 0) {
        *rising_action = PCNT_CHANNEL_EDGE_ACTION_HOLD;
        *falling_action = PCNT_CHANNEL_EDGE_ACTION_INCREASE;
    } else if (strcmp(str, "both") == 0) {
        *rising_action = PCNT_CHANNEL_EDGE_ACTION_INCREASE;
        *falling_action = PCNT_CHANNEL_EDGE_ACTION_INCREASE;
    } else {
        Jim_SetResultFormatted(interp,
            "bad edge \"%s\": should be rising, falling, or both", str);
        return JIM_ERR;
    }
    return JIM_OK;
}

static int pcnt_cmd_init(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    long gpio;
    if (Jim_GetLong(interp, argv[0], &gpio) != JIM_OK) return JIM_ERR;
    if (gpio < 0 || gpio > 48) {
        Jim_SetResultString(interp, "invalid GPIO pin", -1);
        return JIM_ERR;
    }

    pcnt_channel_edge_action_t rising_action = PCNT_CHANNEL_EDGE_ACTION_INCREASE;
    pcnt_channel_edge_action_t falling_action = PCNT_CHANNEL_EDGE_ACTION_HOLD;
    long ctrl_gpio = -1;
    long high_limit = 32767;
    long low_limit = -32768;

    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc) {
            Jim_SetResultString(interp, "missing value for option", -1);
            return JIM_ERR;
        }
        const char *opt = Jim_String(argv[i]);

        if (strcmp(opt, "-edge") == 0) {
            const char *val = Jim_String(argv[i + 1]);
            if (parse_edge(interp, val, &rising_action, &falling_action) != JIM_OK) {
                return JIM_ERR;
            }
        } else if (strcmp(opt, "-ctrl_gpio") == 0) {
            if (Jim_GetLong(interp, argv[i + 1], &ctrl_gpio) != JIM_OK) return JIM_ERR;
        } else if (strcmp(opt, "-high_limit") == 0) {
            if (Jim_GetLong(interp, argv[i + 1], &high_limit) != JIM_OK) return JIM_ERR;
        } else if (strcmp(opt, "-low_limit") == 0) {
            if (Jim_GetLong(interp, argv[i + 1], &low_limit) != JIM_OK) return JIM_ERR;
        } else {
            Jim_SetResultFormatted(interp,
                "unknown option \"%s\": should be -edge, -ctrl_gpio, -high_limit, or -low_limit", opt);
            return JIM_ERR;
        }
    }

    int slot = pcnt_find_free();
    if (slot < 0) {
        Jim_SetResultString(interp, "no free PCNT units", -1);
        return JIM_ERR;
    }

    /* Create PCNT unit */
    pcnt_unit_config_t unit_cfg = {
        .high_limit = (int)high_limit,
        .low_limit = (int)low_limit,
    };

    pcnt_unit_handle_t unit = NULL;
    esp_err_t err = pcnt_new_unit(&unit_cfg, &unit);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "pcnt_new_unit failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    /* Create channel */
    pcnt_chan_config_t chan_cfg = {
        .edge_gpio_num = (int)gpio,
        .level_gpio_num = (ctrl_gpio >= 0) ? (int)ctrl_gpio : -1,
    };

    pcnt_channel_handle_t channel = NULL;
    err = pcnt_new_channel(unit, &chan_cfg, &channel);
    if (err != ESP_OK) {
        pcnt_del_unit(unit);
        Jim_SetResultFormatted(interp, "pcnt_new_channel failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    /* Set edge actions */
    err = pcnt_channel_set_edge_action(channel, rising_action, falling_action);
    if (err != ESP_OK) {
        pcnt_del_channel(channel);
        pcnt_del_unit(unit);
        Jim_SetResultFormatted(interp, "pcnt_channel_set_edge_action failed: %s",
                               esp_err_to_name(err));
        return JIM_ERR;
    }

    /* If control GPIO is specified, set level action (count when high, hold when low) */
    if (ctrl_gpio >= 0) {
        pcnt_channel_set_level_action(channel,
                                       PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                       PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
    }

    /* Enable and start */
    err = pcnt_unit_enable(unit);
    if (err != ESP_OK) {
        pcnt_del_channel(channel);
        pcnt_del_unit(unit);
        Jim_SetResultFormatted(interp, "pcnt_unit_enable failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    err = pcnt_unit_clear_count(unit);
    if (err != ESP_OK) {
        pcnt_unit_disable(unit);
        pcnt_del_channel(channel);
        pcnt_del_unit(unit);
        Jim_SetResultFormatted(interp, "pcnt_unit_clear_count failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    err = pcnt_unit_start(unit);
    if (err != ESP_OK) {
        pcnt_unit_disable(unit);
        pcnt_del_channel(channel);
        pcnt_del_unit(unit);
        Jim_SetResultFormatted(interp, "pcnt_unit_start failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    pcnt_state_t *state = &pcnt_units[slot];
    state->in_use = 1;
    state->gpio = (int)gpio;
    state->unit = unit;
    state->channel = channel;
    state->high_limit = (int)high_limit;
    state->low_limit = (int)low_limit;
    state->paused = 0;
    memset(state->watches, 0, sizeof(state->watches));

    ESP_LOGI(TAG, "PCNT unit %d: gpio=%ld high=%ld low=%ld", slot, gpio, high_limit, low_limit);
    Jim_SetResultInt(interp, slot);
    return JIM_OK;
}

static int pcnt_cmd_read(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    long handle;
    if (Jim_GetLong(interp, argv[0], &handle) != JIM_OK) return JIM_ERR;
    if (handle < 0 || handle >= PCNT_MAX_UNITS || !pcnt_units[handle].in_use) {
        Jim_SetResultString(interp, "invalid PCNT handle", -1);
        return JIM_ERR;
    }

    int count = 0;
    esp_err_t err = pcnt_unit_get_count(pcnt_units[handle].unit, &count);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "pcnt_unit_get_count failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    Jim_SetResultInt(interp, count);
    return JIM_OK;
}

static int pcnt_cmd_clear(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    long handle;
    if (Jim_GetLong(interp, argv[0], &handle) != JIM_OK) return JIM_ERR;
    if (handle < 0 || handle >= PCNT_MAX_UNITS || !pcnt_units[handle].in_use) {
        Jim_SetResultString(interp, "invalid PCNT handle", -1);
        return JIM_ERR;
    }

    esp_err_t err = pcnt_unit_clear_count(pcnt_units[handle].unit);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "pcnt_unit_clear_count failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }
    return JIM_OK;
}

static int pcnt_cmd_pause(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    long handle;
    if (Jim_GetLong(interp, argv[0], &handle) != JIM_OK) return JIM_ERR;
    if (handle < 0 || handle >= PCNT_MAX_UNITS || !pcnt_units[handle].in_use) {
        Jim_SetResultString(interp, "invalid PCNT handle", -1);
        return JIM_ERR;
    }

    pcnt_state_t *state = &pcnt_units[handle];
    if (!state->paused) {
        esp_err_t err = pcnt_unit_stop(state->unit);
        if (err != ESP_OK) {
            Jim_SetResultFormatted(interp, "pcnt_unit_stop failed: %s", esp_err_to_name(err));
            return JIM_ERR;
        }
        state->paused = 1;
    }
    return JIM_OK;
}

static int pcnt_cmd_resume(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    long handle;
    if (Jim_GetLong(interp, argv[0], &handle) != JIM_OK) return JIM_ERR;
    if (handle < 0 || handle >= PCNT_MAX_UNITS || !pcnt_units[handle].in_use) {
        Jim_SetResultString(interp, "invalid PCNT handle", -1);
        return JIM_ERR;
    }

    pcnt_state_t *state = &pcnt_units[handle];
    if (state->paused) {
        esp_err_t err = pcnt_unit_start(state->unit);
        if (err != ESP_OK) {
            Jim_SetResultFormatted(interp, "pcnt_unit_start failed: %s", esp_err_to_name(err));
            return JIM_ERR;
        }
        state->paused = 0;
    }
    return JIM_OK;
}

static int pcnt_cmd_watch(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    long handle;
    if (Jim_GetLong(interp, argv[0], &handle) != JIM_OK) return JIM_ERR;
    if (handle < 0 || handle >= PCNT_MAX_UNITS || !pcnt_units[handle].in_use) {
        Jim_SetResultString(interp, "invalid PCNT handle", -1);
        return JIM_ERR;
    }

    long threshold;
    if (Jim_GetLong(interp, argv[1], &threshold) != JIM_OK) return JIM_ERR;

    /* Parse -callback {proc task} */
    const char *proc_name = NULL;
    const char *task_name = NULL;

    for (int i = 2; i < argc; i += 2) {
        if (i + 1 >= argc) {
            Jim_SetResultString(interp, "missing value for option", -1);
            return JIM_ERR;
        }
        const char *opt = Jim_String(argv[i]);
        if (strcmp(opt, "-callback") == 0) {
            int cb_len = Jim_ListLength(interp, argv[i + 1]);
            if (cb_len != 2) {
                Jim_SetResultString(interp, "-callback requires {proc task}", -1);
                return JIM_ERR;
            }
            proc_name = Jim_String(Jim_ListGetIndex(interp, argv[i + 1], 0));
            task_name = Jim_String(Jim_ListGetIndex(interp, argv[i + 1], 1));
        } else {
            Jim_SetResultFormatted(interp, "unknown option \"%s\": should be -callback", opt);
            return JIM_ERR;
        }
    }

    if (!proc_name || !task_name) {
        Jim_SetResultString(interp, "watch requires -callback {proc task}", -1);
        return JIM_ERR;
    }

    pcnt_state_t *state = &pcnt_units[handle];

    /* Find free watch slot */
    int wslot = -1;
    for (int i = 0; i < PCNT_MAX_WATCHES; i++) {
        if (!state->watches[i].active) {
            wslot = i;
            break;
        }
    }
    if (wslot < 0) {
        Jim_SetResultString(interp, "no free watch slots for this unit", -1);
        return JIM_ERR;
    }

    /* Add watch point */
    esp_err_t err = pcnt_unit_add_watch_point(state->unit, (int)threshold);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "pcnt_unit_add_watch_point failed: %s",
                               esp_err_to_name(err));
        return JIM_ERR;
    }

    /* Register callback (only needs to be done once, but is idempotent) */
    pcnt_event_callbacks_t cbs = {
        .on_reach = pcnt_watch_callback,
    };
    err = pcnt_unit_register_event_callbacks(state->unit, &cbs, state);
    if (err != ESP_OK) {
        pcnt_unit_remove_watch_point(state->unit, (int)threshold);
        Jim_SetResultFormatted(interp, "pcnt_unit_register_event_callbacks failed: %s",
                               esp_err_to_name(err));
        return JIM_ERR;
    }

    state->watches[wslot].active = 1;
    state->watches[wslot].threshold = (int)threshold;
    snprintf(state->watches[wslot].callback_proc,
             sizeof(state->watches[wslot].callback_proc), "%s", proc_name);
    snprintf(state->watches[wslot].callback_task,
             sizeof(state->watches[wslot].callback_task), "%s", task_name);

    ESP_LOGI(TAG, "PCNT unit %ld: watch at %ld -> %s on %s",
             handle, threshold, proc_name, task_name);
    return JIM_OK;
}

static int pcnt_cmd_deinit(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    long handle;
    if (Jim_GetLong(interp, argv[0], &handle) != JIM_OK) return JIM_ERR;
    if (handle < 0 || handle >= PCNT_MAX_UNITS || !pcnt_units[handle].in_use) {
        Jim_SetResultString(interp, "invalid PCNT handle", -1);
        return JIM_ERR;
    }

    pcnt_state_t *state = &pcnt_units[handle];

    /* Remove watch points */
    for (int i = 0; i < PCNT_MAX_WATCHES; i++) {
        if (state->watches[i].active) {
            pcnt_unit_remove_watch_point(state->unit, state->watches[i].threshold);
        }
    }

    pcnt_unit_stop(state->unit);
    pcnt_unit_disable(state->unit);
    pcnt_del_channel(state->channel);
    pcnt_del_unit(state->unit);
    memset(state, 0, sizeof(*state));

    ESP_LOGI(TAG, "PCNT unit %ld deinitialized", handle);
    return JIM_OK;
}

static const jim_subcmd_type pcnt_command_table[] = {
    {   "init",
        "gpio ?-edge rising|falling|both? ?-ctrl_gpio pin? ?-high_limit n? ?-low_limit n?",
        pcnt_cmd_init,
        1, -1,
        /* Description: Initialize a pulse counter on a GPIO pin */
    },
    {   "read",
        "handle",
        pcnt_cmd_read,
        1, 1,
        /* Description: Read current count value */
    },
    {   "clear",
        "handle",
        pcnt_cmd_clear,
        1, 1,
        /* Description: Reset count to zero */
    },
    {   "pause",
        "handle",
        pcnt_cmd_pause,
        1, 1,
        /* Description: Pause counting */
    },
    {   "resume",
        "handle",
        pcnt_cmd_resume,
        1, 1,
        /* Description: Resume counting */
    },
    {   "watch",
        "handle threshold -callback {proc task}",
        pcnt_cmd_watch,
        2, -1,
        /* Description: Set a watch point to fire callback at threshold */
    },
    {   "deinit",
        "handle",
        pcnt_cmd_deinit,
        1, 1,
        /* Description: Deinitialize pulse counter unit */
    },
    { NULL }
};

int Jim_pcntInit(Jim_Interp *interp)
{
    Jim_PackageProvideCheck(interp, "pcnt");
    Jim_RegisterSubCmd(interp, "pcnt", pcnt_command_table, NULL);
    return JIM_OK;
}
