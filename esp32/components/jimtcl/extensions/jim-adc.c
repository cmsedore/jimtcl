/* Jim Tcl ADC Extension for ESP32
 *
 * Provides Tcl commands for ADC oneshot reading:
 *
 *   adc init <unit> ?-atten 0dB|2.5dB|6dB|11dB? ?-width 9|10|11|12|13?
 *   adc read <unit> <channel>           ;# returns raw value
 *   adc voltage <unit> <channel>        ;# returns millivolts (calibrated)
 *   adc deinit <unit>
 */

#include <string.h>
#include <stdlib.h>
#include "jim.h"
#include "jim-subcmd.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"

static const char *TAG = "jim-adc";

#define ADC_MAX_UNITS 2

typedef struct {
    adc_oneshot_unit_handle_t handle;
    adc_atten_t atten;
    adc_bitwidth_t width;
    adc_cali_handle_t cali_handle;
    int cali_valid;
} adc_unit_state_t;

static adc_unit_state_t adc_units[ADC_MAX_UNITS] = { { 0 }, { 0 } };

static int adc_get_unit(Jim_Interp *interp, Jim_Obj *obj, int *unit)
{
    long val;
    if (Jim_GetLong(interp, obj, &val) != JIM_OK) {
        return JIM_ERR;
    }
    if (val < 0 || val >= ADC_MAX_UNITS) {
        Jim_SetResultFormatted(interp, "invalid ADC unit: %ld (must be 0-%d)", val, ADC_MAX_UNITS - 1);
        return JIM_ERR;
    }
    *unit = (int)val;
    return JIM_OK;
}

static int parse_atten(Jim_Interp *interp, const char *str, adc_atten_t *atten)
{
    if (strcmp(str, "0dB") == 0 || strcmp(str, "0db") == 0) {
        *atten = ADC_ATTEN_DB_0;
    } else if (strcmp(str, "2.5dB") == 0 || strcmp(str, "2.5db") == 0) {
        *atten = ADC_ATTEN_DB_2_5;
    } else if (strcmp(str, "6dB") == 0 || strcmp(str, "6db") == 0) {
        *atten = ADC_ATTEN_DB_6;
    } else if (strcmp(str, "11dB") == 0 || strcmp(str, "11db") == 0) {
        *atten = ADC_ATTEN_DB_11;
    } else {
        Jim_SetResultFormatted(interp,
            "bad attenuation \"%s\": should be 0dB, 2.5dB, 6dB, or 11dB", str);
        return JIM_ERR;
    }
    return JIM_OK;
}

static int parse_width(Jim_Interp *interp, const char *str, adc_bitwidth_t *width)
{
    long val;
    /* Manual parse since str is not a Jim_Obj */
    char *end;
    val = strtol(str, &end, 10);
    if (*end != '\0') {
        Jim_SetResultFormatted(interp, "bad width \"%s\": should be 9, 10, 11, 12, or 13", str);
        return JIM_ERR;
    }

    switch (val) {
        case 9:  *width = ADC_BITWIDTH_9;  break;
        case 10: *width = ADC_BITWIDTH_10; break;
        case 11: *width = ADC_BITWIDTH_11; break;
        case 12: *width = ADC_BITWIDTH_12; break;
        case 13: *width = ADC_BITWIDTH_13; break;
        default:
            Jim_SetResultFormatted(interp, "bad width \"%s\": should be 9, 10, 11, 12, or 13", str);
            return JIM_ERR;
    }
    return JIM_OK;
}

static void adc_try_calibration(int unit)
{
    adc_unit_state_t *u = &adc_units[unit];
    u->cali_valid = 0;

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    {
        adc_cali_line_fitting_config_t cali_config = {
            .unit_id = (unit == 0) ? ADC_UNIT_1 : ADC_UNIT_2,
            .atten = u->atten,
            .bitwidth = u->width,
        };
        if (adc_cali_create_scheme_line_fitting(&cali_config, &u->cali_handle) == ESP_OK) {
            u->cali_valid = 1;
            ESP_LOGI(TAG, "ADC unit %d: line fitting calibration enabled", unit);
            return;
        }
    }
#endif

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    {
        adc_cali_curve_fitting_config_t cali_config = {
            .unit_id = (unit == 0) ? ADC_UNIT_1 : ADC_UNIT_2,
            .atten = u->atten,
            .bitwidth = u->width,
        };
        if (adc_cali_create_scheme_curve_fitting(&cali_config, &u->cali_handle) == ESP_OK) {
            u->cali_valid = 1;
            ESP_LOGI(TAG, "ADC unit %d: curve fitting calibration enabled", unit);
            return;
        }
    }
#endif

    ESP_LOGW(TAG, "ADC unit %d: no calibration scheme available", unit);
}

static int adc_cmd_init(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    int unit;
    if (adc_get_unit(interp, argv[0], &unit) != JIM_OK) {
        return JIM_ERR;
    }

    adc_atten_t atten = ADC_ATTEN_DB_11;
    adc_bitwidth_t width = ADC_BITWIDTH_DEFAULT;

    /* Parse optional keyword arguments */
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc) {
            Jim_SetResultString(interp, "missing value for option", -1);
            return JIM_ERR;
        }
        const char *opt = Jim_String(argv[i]);
        const char *val = Jim_String(argv[i + 1]);

        if (strcmp(opt, "-atten") == 0) {
            if (parse_atten(interp, val, &atten) != JIM_OK) return JIM_ERR;
        } else if (strcmp(opt, "-width") == 0) {
            if (parse_width(interp, val, &width) != JIM_OK) return JIM_ERR;
        } else {
            Jim_SetResultFormatted(interp, "unknown option \"%s\": should be -atten or -width", opt);
            return JIM_ERR;
        }
    }

    /* Deinit if already initialized */
    adc_unit_state_t *u = &adc_units[unit];
    if (u->handle != NULL) {
        if (u->cali_valid && u->cali_handle != NULL) {
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
            adc_cali_delete_scheme_line_fitting(u->cali_handle);
#elif ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
            adc_cali_delete_scheme_curve_fitting(u->cali_handle);
#endif
            u->cali_handle = NULL;
            u->cali_valid = 0;
        }
        adc_oneshot_del_unit(u->handle);
        u->handle = NULL;
    }

    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = (unit == 0) ? ADC_UNIT_1 : ADC_UNIT_2,
    };

    esp_err_t err = adc_oneshot_new_unit(&init_cfg, &u->handle);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "adc_oneshot_new_unit failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    u->atten = atten;
    u->width = width;

    /* Attempt calibration */
    adc_try_calibration(unit);

    ESP_LOGI(TAG, "ADC unit %d initialized: atten=%d width=%d cali=%d",
             unit, (int)atten, (int)width, u->cali_valid);
    return JIM_OK;
}

static int adc_cmd_read(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    int unit;
    if (adc_get_unit(interp, argv[0], &unit) != JIM_OK) {
        return JIM_ERR;
    }

    adc_unit_state_t *u = &adc_units[unit];
    if (u->handle == NULL) {
        Jim_SetResultString(interp, "ADC unit not initialized", -1);
        return JIM_ERR;
    }

    long chan;
    if (Jim_GetLong(interp, argv[1], &chan) != JIM_OK) {
        return JIM_ERR;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = u->atten,
        .bitwidth = u->width,
    };
    esp_err_t err = adc_oneshot_config_channel(u->handle, (adc_channel_t)chan, &chan_cfg);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "adc_oneshot_config_channel failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    int raw;
    err = adc_oneshot_read(u->handle, (adc_channel_t)chan, &raw);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "adc_oneshot_read failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    Jim_SetResultInt(interp, raw);
    return JIM_OK;
}

static int adc_cmd_voltage(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    int unit;
    if (adc_get_unit(interp, argv[0], &unit) != JIM_OK) {
        return JIM_ERR;
    }

    adc_unit_state_t *u = &adc_units[unit];
    if (u->handle == NULL) {
        Jim_SetResultString(interp, "ADC unit not initialized", -1);
        return JIM_ERR;
    }

    long chan;
    if (Jim_GetLong(interp, argv[1], &chan) != JIM_OK) {
        return JIM_ERR;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = u->atten,
        .bitwidth = u->width,
    };
    esp_err_t err = adc_oneshot_config_channel(u->handle, (adc_channel_t)chan, &chan_cfg);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "adc_oneshot_config_channel failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    int raw;
    err = adc_oneshot_read(u->handle, (adc_channel_t)chan, &raw);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "adc_oneshot_read failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    if (!u->cali_valid) {
        ESP_LOGW(TAG, "ADC unit %d: no calibration, returning raw value", unit);
        Jim_SetResultInt(interp, raw);
        return JIM_OK;
    }

    int voltage_mv;
    err = adc_cali_raw_to_voltage(u->cali_handle, raw, &voltage_mv);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "adc_cali_raw_to_voltage failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    Jim_SetResultInt(interp, voltage_mv);
    return JIM_OK;
}

static int adc_cmd_deinit(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    int unit;
    if (adc_get_unit(interp, argv[0], &unit) != JIM_OK) {
        return JIM_ERR;
    }

    adc_unit_state_t *u = &adc_units[unit];
    if (u->handle != NULL) {
        if (u->cali_valid && u->cali_handle != NULL) {
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
            adc_cali_delete_scheme_line_fitting(u->cali_handle);
#elif ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
            adc_cali_delete_scheme_curve_fitting(u->cali_handle);
#endif
            u->cali_handle = NULL;
            u->cali_valid = 0;
        }
        adc_oneshot_del_unit(u->handle);
        u->handle = NULL;
    }

    ESP_LOGI(TAG, "ADC unit %d deinitialized", unit);
    return JIM_OK;
}

static const jim_subcmd_type adc_command_table[] = {
    {   "init",
        "unit ?-atten 0dB|2.5dB|6dB|11dB? ?-width 9|10|11|12|13?",
        adc_cmd_init,
        1,
        -1,
        /* Description: Initialize ADC oneshot unit */
    },
    {   "read",
        "unit channel",
        adc_cmd_read,
        2,
        2,
        /* Description: Read raw ADC value from channel */
    },
    {   "voltage",
        "unit channel",
        adc_cmd_voltage,
        2,
        2,
        /* Description: Read calibrated voltage in millivolts from channel */
    },
    {   "deinit",
        "unit",
        adc_cmd_deinit,
        1,
        1,
        /* Description: Deinitialize ADC unit */
    },
    { NULL }
};

int Jim_adcInit(Jim_Interp *interp)
{
    Jim_PackageProvideCheck(interp, "adc");
    Jim_RegisterSubCmd(interp, "adc", adc_command_table, NULL);
    return JIM_OK;
}
