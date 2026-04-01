/* Jim Tcl GPIO Extension for ESP32
 *
 * Provides Tcl commands for GPIO pin control:
 *
 *   gpio mode <pin> input|output|input_output
 *   gpio read <pin>
 *   gpio write <pin> 0|1
 *   gpio pullup <pin> 0|1
 *   gpio pulldown <pin> 0|1
 */

#include <string.h>
#include "jim.h"
#include "jim-subcmd.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "jim-gpio";

static int gpio_get_pin(Jim_Interp *interp, Jim_Obj *obj, gpio_num_t *pin)
{
    long val;
    if (Jim_GetLong(interp, obj, &val) != JIM_OK) {
        return JIM_ERR;
    }
    if (val < 0 || val >= GPIO_NUM_MAX) {
        Jim_SetResultFormatted(interp, "invalid GPIO pin number: %ld", val);
        return JIM_ERR;
    }
    *pin = (gpio_num_t)val;
    return JIM_OK;
}

static int gpio_cmd_mode(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    gpio_num_t pin;
    if (gpio_get_pin(interp, argv[0], &pin) != JIM_OK) {
        return JIM_ERR;
    }

    const char *mode_str = Jim_String(argv[1]);
    gpio_mode_t mode;

    if (strcmp(mode_str, "input") == 0) {
        mode = GPIO_MODE_INPUT;
    }
    else if (strcmp(mode_str, "output") == 0) {
        mode = GPIO_MODE_OUTPUT;
    }
    else if (strcmp(mode_str, "input_output") == 0) {
        mode = GPIO_MODE_INPUT_OUTPUT;
    }
    else if (strcmp(mode_str, "disable") == 0) {
        mode = GPIO_MODE_DISABLE;
    }
    else {
        Jim_SetResultFormatted(interp, "bad mode \"%s\": should be input, output, input_output, or disable", mode_str);
        return JIM_ERR;
    }

    esp_err_t err = gpio_set_direction(pin, mode);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "gpio_set_direction failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    /* Also reset the pin to a known state */
    gpio_reset_pin(pin);
    gpio_set_direction(pin, mode);

    ESP_LOGD(TAG, "GPIO %d set to mode %s", pin, mode_str);
    return JIM_OK;
}

static int gpio_cmd_read(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    gpio_num_t pin;
    if (gpio_get_pin(interp, argv[0], &pin) != JIM_OK) {
        return JIM_ERR;
    }

    int level = gpio_get_level(pin);
    Jim_SetResultInt(interp, level);
    return JIM_OK;
}

static int gpio_cmd_write(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    gpio_num_t pin;
    if (gpio_get_pin(interp, argv[0], &pin) != JIM_OK) {
        return JIM_ERR;
    }

    long level;
    if (Jim_GetLong(interp, argv[1], &level) != JIM_OK) {
        return JIM_ERR;
    }

    esp_err_t err = gpio_set_level(pin, level ? 1 : 0);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "gpio_set_level failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    return JIM_OK;
}

static int gpio_cmd_pullup(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    gpio_num_t pin;
    if (gpio_get_pin(interp, argv[0], &pin) != JIM_OK) {
        return JIM_ERR;
    }

    long enable;
    if (Jim_GetLong(interp, argv[1], &enable) != JIM_OK) {
        return JIM_ERR;
    }

    esp_err_t err;
    if (enable) {
        err = gpio_pullup_en(pin);
    } else {
        err = gpio_pullup_dis(pin);
    }
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "gpio pullup failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    return JIM_OK;
}

static int gpio_cmd_pulldown(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    gpio_num_t pin;
    if (gpio_get_pin(interp, argv[0], &pin) != JIM_OK) {
        return JIM_ERR;
    }

    long enable;
    if (Jim_GetLong(interp, argv[1], &enable) != JIM_OK) {
        return JIM_ERR;
    }

    esp_err_t err;
    if (enable) {
        err = gpio_pulldown_en(pin);
    } else {
        err = gpio_pulldown_dis(pin);
    }
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "gpio pulldown failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    return JIM_OK;
}

static const jim_subcmd_type gpio_command_table[] = {
    {   "mode",
        "pin input|output|input_output|disable",
        gpio_cmd_mode,
        2,
        2,
        /* Description: Set GPIO pin direction */
    },
    {   "read",
        "pin",
        gpio_cmd_read,
        1,
        1,
        /* Description: Read GPIO pin level (returns 0 or 1) */
    },
    {   "write",
        "pin level",
        gpio_cmd_write,
        2,
        2,
        /* Description: Set GPIO pin output level (0 or 1) */
    },
    {   "pullup",
        "pin enable",
        gpio_cmd_pullup,
        2,
        2,
        /* Description: Enable or disable internal pull-up resistor */
    },
    {   "pulldown",
        "pin enable",
        gpio_cmd_pulldown,
        2,
        2,
        /* Description: Enable or disable internal pull-down resistor */
    },
    { NULL }
};

int Jim_gpioInit(Jim_Interp *interp)
{
    Jim_PackageProvideCheck(interp, "gpio");
    Jim_RegisterSubCmd(interp, "gpio", gpio_command_table, NULL);
    return JIM_OK;
}
