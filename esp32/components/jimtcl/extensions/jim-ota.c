/* Jim Tcl OTA Extension for ESP32
 *
 * Provides Tcl commands for over-the-air firmware updates:
 *
 *   ota update <url>
 *   ota status
 *   ota rollback
 *   ota validate
 */

#include <string.h>
#include <stdlib.h>
#include "jim.h"
#include "jim-subcmd.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"

static const char *TAG = "jim-ota";

static int ota_cmd_update(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    const char *url = Jim_String(argv[0]);

    esp_http_client_config_t http_config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    ESP_LOGI(TAG, "Starting OTA update from: %s", url);

    esp_err_t err = esp_https_ota(&ota_config);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "OTA update failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    ESP_LOGI(TAG, "OTA update complete, reboot to activate new firmware");
    Jim_SetResultString(interp, "ok", -1);
    return JIM_OK;
}

static int ota_cmd_status(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) {
        Jim_SetResultString(interp, "cannot determine running partition", -1);
        return JIM_ERR;
    }

    esp_app_desc_t app_desc;
    esp_err_t err = esp_ota_get_partition_description(running, &app_desc);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "cannot read app description: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    Jim_Obj *result = Jim_NewListObj(interp, NULL, 0);

    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "partition", -1));
    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, running->label, -1));

    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "address", -1));
    char addr_buf[16];
    snprintf(addr_buf, sizeof(addr_buf), "0x%08lx", (unsigned long)running->address);
    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, addr_buf, -1));

    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "version", -1));
    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, app_desc.version, -1));

    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "project", -1));
    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, app_desc.project_name, -1));

    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "date", -1));
    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, app_desc.date, -1));

    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "time", -1));
    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, app_desc.time, -1));

    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "idf_ver", -1));
    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, app_desc.idf_ver, -1));

    Jim_SetResult(interp, result);
    return JIM_OK;
}

static int ota_cmd_rollback(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    ESP_LOGW(TAG, "Marking firmware invalid and rebooting to previous version");

    esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
    /* Should not reach here — the device reboots */
    Jim_SetResultFormatted(interp, "rollback failed: %s", esp_err_to_name(err));
    return JIM_ERR;
}

static int ota_cmd_validate(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "validate failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    ESP_LOGI(TAG, "Current firmware marked as valid");
    Jim_SetResultString(interp, "ok", -1);
    return JIM_OK;
}

static const jim_subcmd_type ota_command_table[] = {
    {   "update",
        "url",
        ota_cmd_update,
        1,
        1,
        /* Description: Download and flash firmware from URL */
    },
    {   "status",
        "",
        ota_cmd_status,
        0,
        0,
        /* Description: Return current partition and firmware info */
    },
    {   "rollback",
        "",
        ota_cmd_rollback,
        0,
        0,
        /* Description: Mark firmware invalid and reboot to previous */
    },
    {   "validate",
        "",
        ota_cmd_validate,
        0,
        0,
        /* Description: Mark current firmware as valid */
    },
    { NULL }
};

int Jim_otaInit(Jim_Interp *interp)
{
    Jim_PackageProvideCheck(interp, "ota");
    Jim_RegisterSubCmd(interp, "ota", ota_command_table, NULL);
    return JIM_OK;
}
