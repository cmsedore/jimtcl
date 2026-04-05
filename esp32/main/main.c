/* Jim Tcl on ESP32 - Main Application
 *
 * Boots the ESP32, initializes NVS and the Jim Tcl interpreter,
 * then drops into an interactive REPL over UART.
 *
 * If a startup script exists in NVS (key "boot" in namespace "jimtcl"),
 * it is evaluated before entering interactive mode.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_vfs_dev.h"
#include "driver/uart.h"

#include "jim.h"
#include "jim-esp32.h"

static const char *TAG = "jimtcl-main";

/* Try to load and evaluate a boot script from NVS */
static int load_boot_script(Jim_Interp *interp)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("jimtcl", NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return JIM_OK; /* No boot namespace - that's fine */
    }

    size_t required_size = 0;
    err = nvs_get_str(nvs, "boot", NULL, &required_size);
    if (err != ESP_OK || required_size == 0) {
        nvs_close(nvs);
        return JIM_OK;
    }

    char *script = malloc(required_size);
    if (!script) {
        nvs_close(nvs);
        return JIM_OK;
    }

    err = nvs_get_str(nvs, "boot", script, &required_size);
    nvs_close(nvs);

    if (err != ESP_OK) {
        free(script);
        return JIM_OK;
    }

    ESP_LOGI(TAG, "Executing boot script (%d bytes)", (int)required_size);
    int ret = Jim_Eval(interp, script);
    free(script);

    if (ret == JIM_ERR) {
        const char *errmsg = Jim_String(Jim_GetResult(interp));
        ESP_LOGE(TAG, "Boot script error: %s", errmsg);
    }

    return ret;
}

void app_main(void)
{
    /* Initialize NVS - required for WiFi and our NVS extension */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Install UART driver and configure VFS for linenoise-based REPL.
     * This gives us proper line editing, echo, and history. */
    setvbuf(stdin, NULL, _IONBF, 0);
    uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, 4096, 0, 0, NULL, 0);
    esp_vfs_dev_uart_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);
    esp_vfs_dev_uart_port_set_rx_line_endings(CONFIG_ESP_CONSOLE_UART_NUM, ESP_LINE_ENDINGS_CR);
    esp_vfs_dev_uart_port_set_tx_line_endings(CONFIG_ESP_CONSOLE_UART_NUM, ESP_LINE_ENDINGS_CRLF);

    ESP_LOGI(TAG, "=================================");
    ESP_LOGI(TAG, "  Jim Tcl for ESP32");
    ESP_LOGI(TAG, "  Version %d.%d", JIM_VERSION / 100, JIM_VERSION % 100);
    ESP_LOGI(TAG, "  Free heap: %lu bytes", (unsigned long)esp_get_free_heap_size());
    ESP_LOGI(TAG, "=================================");

    /* Create the main interpreter */
    Jim_Interp *interp = Jim_CreateInterp();
    Jim_RegisterCoreCommands(interp);
    Jim_InitStaticExtensions(interp);
    Jim_Esp32PlatformInit(interp);

    /* Set up useful globals */
    Jim_SetVariableStrWithStr(interp, "jim::argv0", "esp32");
    Jim_SetVariableStrWithStr(interp, JIM_INTERACTIVE, "1");

    /* Try to run boot script from NVS */
    int boot_ret = load_boot_script(interp);
    if (boot_ret == JIM_EXIT) {
        goto cleanup;
    }

    /* Enter interactive mode */
    Jim_Esp32InteractivePrompt(interp);

cleanup:
    Jim_FreeInterp(interp);
    ESP_LOGI(TAG, "Jim Tcl interpreter terminated. Restarting...");
    esp_restart();
}
