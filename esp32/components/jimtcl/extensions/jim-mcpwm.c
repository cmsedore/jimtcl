/* Jim Tcl MCPWM Extension for ESP32
 *
 * Provides Tcl commands for Motor Control PWM:
 *
 *   mcpwm init <group> <gpio_a> ?<gpio_b>? ?-freq hz? ?-resolution hz?
 *   mcpwm duty <handle> <percent_a> ?<percent_b>?
 *   mcpwm brake <handle>
 *   mcpwm servo <gpio> ?-freq 50? ?-min_us 500? ?-max_us 2500?
 *   mcpwm servo set <handle> <angle>
 *   mcpwm deinit <handle>
 */

#include "soc/soc_caps.h"

#if SOC_MCPWM_SUPPORTED

#include <string.h>
#include <stdlib.h>
#include "jim.h"
#include "jim-subcmd.h"
#include "driver/mcpwm_prelude.h"
#include "esp_log.h"

static const char *TAG = "jim-mcpwm";

#define MCPWM_MAX_CHANNELS 4

typedef enum {
    MCPWM_MODE_MOTOR,
    MCPWM_MODE_SERVO,
} mcpwm_mode_t;

typedef struct {
    int in_use;
    mcpwm_mode_t mode;
    int group_id;
    int gpio_a;
    int gpio_b;     /* -1 if single-channel */
    uint32_t freq_hz;
    uint32_t resolution_hz;

    mcpwm_timer_handle_t timer;
    mcpwm_oper_handle_t oper;
    mcpwm_cmpr_handle_t cmpr_a;
    mcpwm_cmpr_handle_t cmpr_b;
    mcpwm_gen_handle_t gen_a;
    mcpwm_gen_handle_t gen_b;

    /* Servo-specific */
    uint32_t servo_min_us;
    uint32_t servo_max_us;
    uint32_t timer_period_ticks;  /* period in ticks for duty calculation */
} mcpwm_state_t;

static mcpwm_state_t mcpwm_channels[MCPWM_MAX_CHANNELS] = { 0 };

static int mcpwm_find_free(void)
{
    for (int i = 0; i < MCPWM_MAX_CHANNELS; i++) {
        if (!mcpwm_channels[i].in_use) return i;
    }
    return -1;
}

static int mcpwm_cmd_init(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    long group, gpio_a;
    if (Jim_GetLong(interp, argv[0], &group) != JIM_OK) return JIM_ERR;
    if (Jim_GetLong(interp, argv[1], &gpio_a) != JIM_OK) return JIM_ERR;

    if (group < 0 || group > 1) {
        Jim_SetResultString(interp, "group must be 0 or 1", -1);
        return JIM_ERR;
    }
    if (gpio_a < 0 || gpio_a > 48) {
        Jim_SetResultString(interp, "invalid GPIO pin", -1);
        return JIM_ERR;
    }

    long gpio_b = -1;
    long freq = 25000;         /* 25kHz default for motor */
    long resolution = 10000000; /* 10MHz default */
    int arg_start = 2;

    /* Check if next arg is gpio_b (a number, not an option) */
    if (argc > 2) {
        const char *s = Jim_String(argv[2]);
        if (s[0] != '-') {
            if (Jim_GetLong(interp, argv[2], &gpio_b) != JIM_OK) return JIM_ERR;
            if (gpio_b < 0 || gpio_b > 48) {
                Jim_SetResultString(interp, "invalid GPIO pin for gpio_b", -1);
                return JIM_ERR;
            }
            arg_start = 3;
        }
    }

    for (int i = arg_start; i < argc; i += 2) {
        if (i + 1 >= argc) {
            Jim_SetResultString(interp, "missing value for option", -1);
            return JIM_ERR;
        }
        const char *opt = Jim_String(argv[i]);
        long val;
        if (Jim_GetLong(interp, argv[i + 1], &val) != JIM_OK) return JIM_ERR;

        if (strcmp(opt, "-freq") == 0) {
            freq = val;
        } else if (strcmp(opt, "-resolution") == 0) {
            resolution = val;
        } else {
            Jim_SetResultFormatted(interp,
                "unknown option \"%s\": should be -freq or -resolution", opt);
            return JIM_ERR;
        }
    }

    int slot = mcpwm_find_free();
    if (slot < 0) {
        Jim_SetResultString(interp, "no free MCPWM channels", -1);
        return JIM_ERR;
    }

    /* Create timer */
    uint32_t period_ticks = (uint32_t)resolution / (uint32_t)freq;
    mcpwm_timer_config_t timer_cfg = {
        .group_id = (int)group,
        .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT,
        .resolution_hz = (uint32_t)resolution,
        .count_mode = MCPWM_TIMER_COUNT_MODE_UP,
        .period_ticks = period_ticks,
    };

    mcpwm_timer_handle_t timer = NULL;
    esp_err_t err = mcpwm_new_timer(&timer_cfg, &timer);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "mcpwm_new_timer failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    /* Create operator */
    mcpwm_operator_config_t oper_cfg = {
        .group_id = (int)group,
    };
    mcpwm_oper_handle_t oper = NULL;
    err = mcpwm_new_operator(&oper_cfg, &oper);
    if (err != ESP_OK) {
        mcpwm_del_timer(timer);
        Jim_SetResultFormatted(interp, "mcpwm_new_operator failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    /* Connect operator to timer */
    err = mcpwm_operator_connect_timer(oper, timer);
    if (err != ESP_OK) {
        mcpwm_del_operator(oper);
        mcpwm_del_timer(timer);
        Jim_SetResultFormatted(interp, "mcpwm_operator_connect_timer failed: %s",
                               esp_err_to_name(err));
        return JIM_ERR;
    }

    /* Create comparator A */
    mcpwm_comparator_config_t cmpr_cfg = {
        .flags.update_cmp_on_tez = true,
    };
    mcpwm_cmpr_handle_t cmpr_a = NULL;
    err = mcpwm_new_comparator(oper, &cmpr_cfg, &cmpr_a);
    if (err != ESP_OK) {
        mcpwm_del_operator(oper);
        mcpwm_del_timer(timer);
        Jim_SetResultFormatted(interp, "mcpwm_new_comparator failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    /* Create generator A */
    mcpwm_generator_config_t gen_cfg_a = {
        .gen_gpio_num = (int)gpio_a,
    };
    mcpwm_gen_handle_t gen_a = NULL;
    err = mcpwm_new_generator(oper, &gen_cfg_a, &gen_a);
    if (err != ESP_OK) {
        mcpwm_del_comparator(cmpr_a);
        mcpwm_del_operator(oper);
        mcpwm_del_timer(timer);
        Jim_SetResultFormatted(interp, "mcpwm_new_generator failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    /* Set generator actions: high on timer empty, low on compare */
    mcpwm_generator_set_action_on_timer_event(gen_a,
        MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_HIGH));
    mcpwm_generator_set_action_on_compare_event(gen_a,
        MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, cmpr_a, MCPWM_GEN_ACTION_LOW));

    /* Set initial duty to 0 */
    mcpwm_comparator_set_compare_value(cmpr_a, 0);

    /* Optional: generator B */
    mcpwm_cmpr_handle_t cmpr_b = NULL;
    mcpwm_gen_handle_t gen_b = NULL;

    if (gpio_b >= 0) {
        err = mcpwm_new_comparator(oper, &cmpr_cfg, &cmpr_b);
        if (err != ESP_OK) {
            mcpwm_del_generator(gen_a);
            mcpwm_del_comparator(cmpr_a);
            mcpwm_del_operator(oper);
            mcpwm_del_timer(timer);
            Jim_SetResultFormatted(interp, "mcpwm_new_comparator (B) failed: %s",
                                   esp_err_to_name(err));
            return JIM_ERR;
        }

        mcpwm_generator_config_t gen_cfg_b = {
            .gen_gpio_num = (int)gpio_b,
        };
        err = mcpwm_new_generator(oper, &gen_cfg_b, &gen_b);
        if (err != ESP_OK) {
            mcpwm_del_comparator(cmpr_b);
            mcpwm_del_generator(gen_a);
            mcpwm_del_comparator(cmpr_a);
            mcpwm_del_operator(oper);
            mcpwm_del_timer(timer);
            Jim_SetResultFormatted(interp, "mcpwm_new_generator (B) failed: %s",
                                   esp_err_to_name(err));
            return JIM_ERR;
        }

        mcpwm_generator_set_action_on_timer_event(gen_b,
            MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_HIGH));
        mcpwm_generator_set_action_on_compare_event(gen_b,
            MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, cmpr_b, MCPWM_GEN_ACTION_LOW));

        mcpwm_comparator_set_compare_value(cmpr_b, 0);
    }

    /* Enable and start timer */
    err = mcpwm_timer_enable(timer);
    if (err != ESP_OK) {
        if (gen_b) mcpwm_del_generator(gen_b);
        if (cmpr_b) mcpwm_del_comparator(cmpr_b);
        mcpwm_del_generator(gen_a);
        mcpwm_del_comparator(cmpr_a);
        mcpwm_del_operator(oper);
        mcpwm_del_timer(timer);
        Jim_SetResultFormatted(interp, "mcpwm_timer_enable failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    err = mcpwm_timer_start_stop(timer, MCPWM_TIMER_START_NO_STOP);
    if (err != ESP_OK) {
        mcpwm_timer_disable(timer);
        if (gen_b) mcpwm_del_generator(gen_b);
        if (cmpr_b) mcpwm_del_comparator(cmpr_b);
        mcpwm_del_generator(gen_a);
        mcpwm_del_comparator(cmpr_a);
        mcpwm_del_operator(oper);
        mcpwm_del_timer(timer);
        Jim_SetResultFormatted(interp, "mcpwm_timer_start_stop failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    mcpwm_state_t *state = &mcpwm_channels[slot];
    state->in_use = 1;
    state->mode = MCPWM_MODE_MOTOR;
    state->group_id = (int)group;
    state->gpio_a = (int)gpio_a;
    state->gpio_b = (int)gpio_b;
    state->freq_hz = (uint32_t)freq;
    state->resolution_hz = (uint32_t)resolution;
    state->timer = timer;
    state->oper = oper;
    state->cmpr_a = cmpr_a;
    state->cmpr_b = cmpr_b;
    state->gen_a = gen_a;
    state->gen_b = gen_b;
    state->timer_period_ticks = period_ticks;

    ESP_LOGI(TAG, "MCPWM slot %d: group=%ld gpio_a=%ld gpio_b=%ld freq=%ld",
             slot, group, gpio_a, gpio_b, freq);
    Jim_SetResultInt(interp, slot);
    return JIM_OK;
}

static int mcpwm_cmd_duty(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    long handle;
    if (Jim_GetLong(interp, argv[0], &handle) != JIM_OK) return JIM_ERR;
    if (handle < 0 || handle >= MCPWM_MAX_CHANNELS || !mcpwm_channels[handle].in_use) {
        Jim_SetResultString(interp, "invalid MCPWM handle", -1);
        return JIM_ERR;
    }

    long percent_a;
    if (Jim_GetLong(interp, argv[1], &percent_a) != JIM_OK) return JIM_ERR;
    if (percent_a < 0 || percent_a > 100) {
        Jim_SetResultString(interp, "duty must be 0-100", -1);
        return JIM_ERR;
    }

    mcpwm_state_t *state = &mcpwm_channels[handle];
    uint32_t cmp_a = (uint32_t)((percent_a * state->timer_period_ticks) / 100);
    esp_err_t err = mcpwm_comparator_set_compare_value(state->cmpr_a, cmp_a);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "mcpwm_comparator_set_compare_value failed: %s",
                               esp_err_to_name(err));
        return JIM_ERR;
    }

    if (argc > 2 && state->cmpr_b) {
        long percent_b;
        if (Jim_GetLong(interp, argv[2], &percent_b) != JIM_OK) return JIM_ERR;
        if (percent_b < 0 || percent_b > 100) {
            Jim_SetResultString(interp, "duty must be 0-100", -1);
            return JIM_ERR;
        }
        uint32_t cmp_b = (uint32_t)((percent_b * state->timer_period_ticks) / 100);
        err = mcpwm_comparator_set_compare_value(state->cmpr_b, cmp_b);
        if (err != ESP_OK) {
            Jim_SetResultFormatted(interp, "mcpwm_comparator_set_compare_value (B) failed: %s",
                                   esp_err_to_name(err));
            return JIM_ERR;
        }
    }
    return JIM_OK;
}

static int mcpwm_cmd_brake(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    long handle;
    if (Jim_GetLong(interp, argv[0], &handle) != JIM_OK) return JIM_ERR;
    if (handle < 0 || handle >= MCPWM_MAX_CHANNELS || !mcpwm_channels[handle].in_use) {
        Jim_SetResultString(interp, "invalid MCPWM handle", -1);
        return JIM_ERR;
    }

    mcpwm_state_t *state = &mcpwm_channels[handle];

    /* Set both outputs to 0% duty (both low) */
    mcpwm_comparator_set_compare_value(state->cmpr_a, 0);
    if (state->cmpr_b) {
        mcpwm_comparator_set_compare_value(state->cmpr_b, 0);
    }

    ESP_LOGI(TAG, "MCPWM slot %ld: brake engaged", handle);
    return JIM_OK;
}

/* ===== Servo subcommands ===== */

static int mcpwm_servo_cmd_init(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    long gpio;
    if (Jim_GetLong(interp, argv[0], &gpio) != JIM_OK) return JIM_ERR;
    if (gpio < 0 || gpio > 48) {
        Jim_SetResultString(interp, "invalid GPIO pin", -1);
        return JIM_ERR;
    }

    long freq = 50;
    long min_us = 500;
    long max_us = 2500;

    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc) {
            Jim_SetResultString(interp, "missing value for option", -1);
            return JIM_ERR;
        }
        const char *opt = Jim_String(argv[i]);
        long val;
        if (Jim_GetLong(interp, argv[i + 1], &val) != JIM_OK) return JIM_ERR;

        if (strcmp(opt, "-freq") == 0) {
            freq = val;
        } else if (strcmp(opt, "-min_us") == 0) {
            min_us = val;
        } else if (strcmp(opt, "-max_us") == 0) {
            max_us = val;
        } else {
            Jim_SetResultFormatted(interp,
                "unknown option \"%s\": should be -freq, -min_us, or -max_us", opt);
            return JIM_ERR;
        }
    }

    int slot = mcpwm_find_free();
    if (slot < 0) {
        Jim_SetResultString(interp, "no free MCPWM channels", -1);
        return JIM_ERR;
    }

    /* Servo: use 1MHz resolution for microsecond-level control */
    uint32_t resolution = 1000000;  /* 1MHz */
    uint32_t period_ticks = resolution / (uint32_t)freq;

    mcpwm_timer_config_t timer_cfg = {
        .group_id = 0,
        .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT,
        .resolution_hz = resolution,
        .count_mode = MCPWM_TIMER_COUNT_MODE_UP,
        .period_ticks = period_ticks,
    };

    mcpwm_timer_handle_t timer = NULL;
    esp_err_t err = mcpwm_new_timer(&timer_cfg, &timer);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "mcpwm_new_timer failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    mcpwm_operator_config_t oper_cfg = {
        .group_id = 0,
    };
    mcpwm_oper_handle_t oper = NULL;
    err = mcpwm_new_operator(&oper_cfg, &oper);
    if (err != ESP_OK) {
        mcpwm_del_timer(timer);
        Jim_SetResultFormatted(interp, "mcpwm_new_operator failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    mcpwm_operator_connect_timer(oper, timer);

    mcpwm_comparator_config_t cmpr_cfg = {
        .flags.update_cmp_on_tez = true,
    };
    mcpwm_cmpr_handle_t cmpr = NULL;
    err = mcpwm_new_comparator(oper, &cmpr_cfg, &cmpr);
    if (err != ESP_OK) {
        mcpwm_del_operator(oper);
        mcpwm_del_timer(timer);
        Jim_SetResultFormatted(interp, "mcpwm_new_comparator failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    mcpwm_generator_config_t gen_cfg = {
        .gen_gpio_num = (int)gpio,
    };
    mcpwm_gen_handle_t gen = NULL;
    err = mcpwm_new_generator(oper, &gen_cfg, &gen);
    if (err != ESP_OK) {
        mcpwm_del_comparator(cmpr);
        mcpwm_del_operator(oper);
        mcpwm_del_timer(timer);
        Jim_SetResultFormatted(interp, "mcpwm_new_generator failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    /* Servo: high on timer empty, low on compare match */
    mcpwm_generator_set_action_on_timer_event(gen,
        MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_HIGH));
    mcpwm_generator_set_action_on_compare_event(gen,
        MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, cmpr, MCPWM_GEN_ACTION_LOW));

    /* Center servo at 90 degrees */
    uint32_t center_us = (uint32_t)((min_us + max_us) / 2);
    mcpwm_comparator_set_compare_value(cmpr, center_us);

    mcpwm_timer_enable(timer);
    mcpwm_timer_start_stop(timer, MCPWM_TIMER_START_NO_STOP);

    mcpwm_state_t *state = &mcpwm_channels[slot];
    state->in_use = 1;
    state->mode = MCPWM_MODE_SERVO;
    state->group_id = 0;
    state->gpio_a = (int)gpio;
    state->gpio_b = -1;
    state->freq_hz = (uint32_t)freq;
    state->resolution_hz = resolution;
    state->timer = timer;
    state->oper = oper;
    state->cmpr_a = cmpr;
    state->cmpr_b = NULL;
    state->gen_a = gen;
    state->gen_b = NULL;
    state->servo_min_us = (uint32_t)min_us;
    state->servo_max_us = (uint32_t)max_us;
    state->timer_period_ticks = period_ticks;

    ESP_LOGI(TAG, "Servo slot %d: gpio=%ld freq=%ld min=%ld max=%ld",
             slot, gpio, freq, min_us, max_us);
    Jim_SetResultInt(interp, slot);
    return JIM_OK;
}

static int mcpwm_servo_cmd_set(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    long handle, angle;
    if (Jim_GetLong(interp, argv[0], &handle) != JIM_OK) return JIM_ERR;
    if (Jim_GetLong(interp, argv[1], &angle) != JIM_OK) return JIM_ERR;

    if (handle < 0 || handle >= MCPWM_MAX_CHANNELS || !mcpwm_channels[handle].in_use) {
        Jim_SetResultString(interp, "invalid MCPWM handle", -1);
        return JIM_ERR;
    }
    if (mcpwm_channels[handle].mode != MCPWM_MODE_SERVO) {
        Jim_SetResultString(interp, "handle is not a servo", -1);
        return JIM_ERR;
    }
    if (angle < 0 || angle > 180) {
        Jim_SetResultString(interp, "angle must be 0-180", -1);
        return JIM_ERR;
    }

    mcpwm_state_t *state = &mcpwm_channels[handle];
    /* Map angle (0-180) to pulse width (min_us - max_us) */
    uint32_t pulse_us = state->servo_min_us +
                        (uint32_t)((angle * (state->servo_max_us - state->servo_min_us)) / 180);

    /* At 1MHz resolution, ticks == microseconds */
    esp_err_t err = mcpwm_comparator_set_compare_value(state->cmpr_a, pulse_us);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "mcpwm_comparator_set_compare_value failed: %s",
                               esp_err_to_name(err));
        return JIM_ERR;
    }
    return JIM_OK;
}

static int mcpwm_cmd_deinit(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    long handle;
    if (Jim_GetLong(interp, argv[0], &handle) != JIM_OK) return JIM_ERR;
    if (handle < 0 || handle >= MCPWM_MAX_CHANNELS || !mcpwm_channels[handle].in_use) {
        Jim_SetResultString(interp, "invalid MCPWM handle", -1);
        return JIM_ERR;
    }

    mcpwm_state_t *state = &mcpwm_channels[handle];

    mcpwm_timer_start_stop(state->timer, MCPWM_TIMER_STOP_FULL);
    mcpwm_timer_disable(state->timer);

    if (state->gen_b) mcpwm_del_generator(state->gen_b);
    if (state->cmpr_b) mcpwm_del_comparator(state->cmpr_b);
    mcpwm_del_generator(state->gen_a);
    mcpwm_del_comparator(state->cmpr_a);
    mcpwm_del_operator(state->oper);
    mcpwm_del_timer(state->timer);
    memset(state, 0, sizeof(*state));

    ESP_LOGI(TAG, "MCPWM slot %ld deinitialized", handle);
    return JIM_OK;
}

/* ===== Top-level "mcpwm" dispatch ===== */

static const jim_subcmd_type mcpwm_servo_table[] = {
    {   "set",
        "handle angle",
        mcpwm_servo_cmd_set,
        2, 2,
    },
    { NULL }
};

static int mcpwm_cmd(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc < 2) {
        Jim_SetResultString(interp,
            "wrong # args: should be \"mcpwm init|duty|brake|servo|deinit ...\"", -1);
        return JIM_ERR;
    }

    const char *subcmd = Jim_String(argv[1]);

    if (strcmp(subcmd, "init") == 0) {
        return mcpwm_cmd_init(interp, argc - 2, argv + 2);
    } else if (strcmp(subcmd, "duty") == 0) {
        if (argc < 4) {
            Jim_SetResultString(interp,
                "wrong # args: should be \"mcpwm duty handle percent_a ?percent_b?\"", -1);
            return JIM_ERR;
        }
        return mcpwm_cmd_duty(interp, argc - 2, argv + 2);
    } else if (strcmp(subcmd, "brake") == 0) {
        if (argc < 3) {
            Jim_SetResultString(interp, "wrong # args: should be \"mcpwm brake handle\"", -1);
            return JIM_ERR;
        }
        return mcpwm_cmd_brake(interp, argc - 2, argv + 2);
    } else if (strcmp(subcmd, "servo") == 0) {
        if (argc < 3) {
            Jim_SetResultString(interp,
                "wrong # args: should be \"mcpwm servo <gpio>|set ...\"", -1);
            return JIM_ERR;
        }
        /* Check if it's "mcpwm servo set <handle> <angle>" */
        const char *sub2 = Jim_String(argv[2]);
        if (strcmp(sub2, "set") == 0) {
            { const jim_subcmd_type *ct = Jim_ParseSubCmd(interp, mcpwm_servo_table, argc - 2, argv + 2); return Jim_CallSubCmd(interp, ct, argc - 2, argv + 2); }
        }
        /* Otherwise it's "mcpwm servo <gpio> ?opts?" (init) */
        return mcpwm_servo_cmd_init(interp, argc - 2, argv + 2);
    } else if (strcmp(subcmd, "deinit") == 0) {
        if (argc < 3) {
            Jim_SetResultString(interp, "wrong # args: should be \"mcpwm deinit handle\"", -1);
            return JIM_ERR;
        }
        return mcpwm_cmd_deinit(interp, argc - 2, argv + 2);
    } else {
        Jim_SetResultFormatted(interp,
            "unknown subcommand \"%s\": should be init, duty, brake, servo, or deinit", subcmd);
        return JIM_ERR;
    }
}

int Jim_mcpwmInit(Jim_Interp *interp)
{
    Jim_PackageProvideCheck(interp, "mcpwm");
    Jim_CreateCommand(interp, "mcpwm", mcpwm_cmd, NULL, NULL);
    return JIM_OK;
}

#endif /* SOC_MCPWM_SUPPORTED */
