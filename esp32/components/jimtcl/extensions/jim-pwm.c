/* Jim Tcl PWM/LEDC Extension for ESP32
 *
 * Provides Tcl commands for PWM output via the LEDC peripheral:
 *
 *   pwm init <pin> ?-freq hz? ?-duty percent? ?-channel 0-7? ?-resolution 8-14?
 *   pwm duty <pin> <percent>            ;# 0-100
 *   pwm freq <pin> <hz>
 *   pwm stop <pin>
 */

#include <string.h>
#include <stdlib.h>
#include "jim.h"
#include "jim-subcmd.h"
#include "driver/ledc.h"
#include "esp_log.h"

static const char *TAG = "jim-pwm";

#define PWM_MAX_CHANNELS 8

typedef struct {
    int in_use;
    int pin;
    ledc_channel_t channel;
    ledc_timer_t timer;
    uint32_t freq;
    int resolution;
} pwm_channel_t;

static pwm_channel_t pwm_channels[PWM_MAX_CHANNELS] = { 0 };

/* Find a channel slot by GPIO pin, or -1 if not found */
static int pwm_find_by_pin(int pin)
{
    for (int i = 0; i < PWM_MAX_CHANNELS; i++) {
        if (pwm_channels[i].in_use && pwm_channels[i].pin == pin) {
            return i;
        }
    }
    return -1;
}

/* Find first free channel slot, or -1 */
static int pwm_find_free(void)
{
    for (int i = 0; i < PWM_MAX_CHANNELS; i++) {
        if (!pwm_channels[i].in_use) return i;
    }
    return -1;
}

static int pwm_get_pin(Jim_Interp *interp, Jim_Obj *obj, int *pin)
{
    long val;
    if (Jim_GetLong(interp, obj, &val) != JIM_OK) {
        return JIM_ERR;
    }
    if (val < 0 || val > 48) {
        Jim_SetResultFormatted(interp, "invalid GPIO pin: %ld", val);
        return JIM_ERR;
    }
    *pin = (int)val;
    return JIM_OK;
}

static uint32_t percent_to_duty(int percent, int resolution)
{
    uint32_t max_duty = (1U << resolution) - 1;
    if (percent <= 0) return 0;
    if (percent >= 100) return max_duty;
    return (uint32_t)((percent * max_duty) / 100);
}

static int pwm_cmd_init(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    int pin;
    if (pwm_get_pin(interp, argv[0], &pin) != JIM_OK) {
        return JIM_ERR;
    }

    long freq = 5000;
    long duty_percent = 0;
    long channel = -1;
    long resolution = 13;

    /* Parse optional keyword arguments */
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc) {
            Jim_SetResultString(interp, "missing value for option", -1);
            return JIM_ERR;
        }
        const char *opt = Jim_String(argv[i]);
        long val;
        if (Jim_GetLong(interp, argv[i + 1], &val) != JIM_OK) {
            return JIM_ERR;
        }

        if (strcmp(opt, "-freq") == 0) {
            freq = val;
        } else if (strcmp(opt, "-duty") == 0) {
            duty_percent = val;
        } else if (strcmp(opt, "-channel") == 0) {
            channel = val;
        } else if (strcmp(opt, "-resolution") == 0) {
            resolution = val;
        } else {
            Jim_SetResultFormatted(interp,
                "unknown option \"%s\": should be -freq, -duty, -channel, or -resolution", opt);
            return JIM_ERR;
        }
    }

    if (freq <= 0) {
        Jim_SetResultString(interp, "frequency must be positive", -1);
        return JIM_ERR;
    }
    if (duty_percent < 0 || duty_percent > 100) {
        Jim_SetResultString(interp, "duty must be 0-100", -1);
        return JIM_ERR;
    }
    if (resolution < 8 || resolution > 14) {
        Jim_SetResultString(interp, "resolution must be 8-14", -1);
        return JIM_ERR;
    }

    /* Find or allocate a channel slot */
    int slot = pwm_find_by_pin(pin);
    if (slot < 0) {
        if (channel >= 0 && channel < PWM_MAX_CHANNELS) {
            /* User requested a specific channel */
            if (pwm_channels[channel].in_use) {
                Jim_SetResultFormatted(interp, "channel %ld already in use by pin %d",
                                       channel, pwm_channels[channel].pin);
                return JIM_ERR;
            }
            slot = (int)channel;
        } else {
            slot = pwm_find_free();
        }
    }
    if (slot < 0) {
        Jim_SetResultString(interp, "no free PWM channels available", -1);
        return JIM_ERR;
    }

    /* Use the slot index as both channel and timer number */
    ledc_channel_t ledc_chan = (ledc_channel_t)slot;
    ledc_timer_t ledc_timer = (ledc_timer_t)(slot % LEDC_TIMER_MAX);

    /* Configure timer */
    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = ledc_timer,
        .duty_resolution = (ledc_timer_bit_t)resolution,
        .freq_hz = (uint32_t)freq,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_cfg);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "ledc_timer_config failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    /* Configure channel */
    uint32_t duty_val = percent_to_duty((int)duty_percent, (int)resolution);
    ledc_channel_config_t chan_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = ledc_chan,
        .timer_sel = ledc_timer,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = pin,
        .duty = duty_val,
        .hpoint = 0,
    };
    err = ledc_channel_config(&chan_cfg);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "ledc_channel_config failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    /* Store state */
    pwm_channels[slot].in_use = 1;
    pwm_channels[slot].pin = pin;
    pwm_channels[slot].channel = ledc_chan;
    pwm_channels[slot].timer = ledc_timer;
    pwm_channels[slot].freq = (uint32_t)freq;
    pwm_channels[slot].resolution = (int)resolution;

    ESP_LOGI(TAG, "PWM pin %d: channel=%d freq=%ld duty=%ld%% resolution=%ld",
             pin, slot, freq, duty_percent, resolution);
    Jim_SetResultInt(interp, slot);
    return JIM_OK;
}

static int pwm_cmd_duty(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    int pin;
    if (pwm_get_pin(interp, argv[0], &pin) != JIM_OK) {
        return JIM_ERR;
    }

    int slot = pwm_find_by_pin(pin);
    if (slot < 0) {
        Jim_SetResultFormatted(interp, "pin %d not initialized for PWM", pin);
        return JIM_ERR;
    }

    long percent;
    if (Jim_GetLong(interp, argv[1], &percent) != JIM_OK) {
        return JIM_ERR;
    }
    if (percent < 0 || percent > 100) {
        Jim_SetResultString(interp, "duty must be 0-100", -1);
        return JIM_ERR;
    }

    pwm_channel_t *ch = &pwm_channels[slot];
    uint32_t duty_val = percent_to_duty((int)percent, ch->resolution);

    esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE, ch->channel, duty_val);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "ledc_set_duty failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }
    err = ledc_update_duty(LEDC_LOW_SPEED_MODE, ch->channel);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "ledc_update_duty failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    return JIM_OK;
}

static int pwm_cmd_freq(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    int pin;
    if (pwm_get_pin(interp, argv[0], &pin) != JIM_OK) {
        return JIM_ERR;
    }

    int slot = pwm_find_by_pin(pin);
    if (slot < 0) {
        Jim_SetResultFormatted(interp, "pin %d not initialized for PWM", pin);
        return JIM_ERR;
    }

    long hz;
    if (Jim_GetLong(interp, argv[1], &hz) != JIM_OK) {
        return JIM_ERR;
    }
    if (hz <= 0) {
        Jim_SetResultString(interp, "frequency must be positive", -1);
        return JIM_ERR;
    }

    pwm_channel_t *ch = &pwm_channels[slot];
    esp_err_t err = ledc_set_freq(LEDC_LOW_SPEED_MODE, ch->timer, (uint32_t)hz);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "ledc_set_freq failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    ch->freq = (uint32_t)hz;
    return JIM_OK;
}

static int pwm_cmd_stop(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    int pin;
    if (pwm_get_pin(interp, argv[0], &pin) != JIM_OK) {
        return JIM_ERR;
    }

    int slot = pwm_find_by_pin(pin);
    if (slot < 0) {
        Jim_SetResultFormatted(interp, "pin %d not initialized for PWM", pin);
        return JIM_ERR;
    }

    pwm_channel_t *ch = &pwm_channels[slot];
    esp_err_t err = ledc_stop(LEDC_LOW_SPEED_MODE, ch->channel, 0);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "ledc_stop failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    ch->in_use = 0;
    ESP_LOGI(TAG, "PWM stopped on pin %d (channel %d freed)", pin, slot);
    return JIM_OK;
}

static const jim_subcmd_type pwm_command_table[] = {
    {   "init",
        "pin ?-freq hz? ?-duty percent? ?-channel 0-7? ?-resolution 8-14?",
        pwm_cmd_init,
        1,
        -1,
        /* Description: Initialize PWM output on a GPIO pin */
    },
    {   "duty",
        "pin percent",
        pwm_cmd_duty,
        2,
        2,
        /* Description: Set PWM duty cycle (0-100%) */
    },
    {   "freq",
        "pin hz",
        pwm_cmd_freq,
        2,
        2,
        /* Description: Set PWM frequency in Hz */
    },
    {   "stop",
        "pin",
        pwm_cmd_stop,
        1,
        1,
        /* Description: Stop PWM output and free the channel */
    },
    { NULL }
};

int Jim_pwmInit(Jim_Interp *interp)
{
    Jim_PackageProvideCheck(interp, "pwm");
    Jim_RegisterSubCmd(interp, "pwm", pwm_command_table, NULL);
    return JIM_OK;
}
