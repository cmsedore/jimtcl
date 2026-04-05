/* Jim Tcl ESP32 Platform Shim
 *
 * Provides platform abstractions required by jim.c and initializes
 * the interpreter for the ESP32 environment.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "jim.h"
#include "jim-esp32.h"
#include "linenoise/linenoise.h"

static const char *TAG = "jimtcl";

/* ---------------------------------------------------------------------------
 * Memory allocator using ESP-IDF heap
 * ---------------------------------------------------------------------------*/

static void *JimEsp32Allocator(void *ptr, size_t size)
{
    if (size == 0) {
        if (ptr) {
            heap_caps_free(ptr);
        }
        return NULL;
    }
    if (ptr) {
        return heap_caps_realloc(ptr, size, MALLOC_CAP_DEFAULT);
    }
    return heap_caps_malloc(size, MALLOC_CAP_DEFAULT);
}

/* ---------------------------------------------------------------------------
 * Temp file stub
 * ---------------------------------------------------------------------------*/

int Jim_MakeTempFile(Jim_Interp *interp, const char *filename_template, int unlink_file)
{
    (void)filename_template;
    (void)unlink_file;
    Jim_SetResultString(interp, "temp files not supported on ESP32", -1);
    return -1;
}

/* ---------------------------------------------------------------------------
 * ESP32 info command: [esp32 info], [esp32 restart], [esp32 heap]
 * ---------------------------------------------------------------------------*/

static int Jim_Esp32InfoCmd(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc < 2) {
        Jim_SetResultString(interp, "wrong # args: should be \"esp32 subcommand ?args?\"", -1);
        return JIM_ERR;
    }

    const char *subcmd = Jim_String(argv[1]);

    if (strcmp(subcmd, "heap") == 0) {
        /* Return dict with heap info */
        Jim_Obj *dict = Jim_NewListObj(interp, NULL, 0);
        Jim_ListAppendElement(interp, dict, Jim_NewStringObj(interp, "free", -1));
        Jim_ListAppendElement(interp, dict, Jim_NewIntObj(interp, (jim_wide)esp_get_free_heap_size()));
        Jim_ListAppendElement(interp, dict, Jim_NewStringObj(interp, "minimum", -1));
        Jim_ListAppendElement(interp, dict, Jim_NewIntObj(interp, (jim_wide)esp_get_minimum_free_heap_size()));
        Jim_SetResult(interp, dict);
        return JIM_OK;
    }
    else if (strcmp(subcmd, "restart") == 0) {
        ESP_LOGW(TAG, "Restarting ESP32 via Tcl command");
        esp_restart();
        /* Never reached */
        return JIM_OK;
    }
    else if (strcmp(subcmd, "info") == 0) {
        esp_chip_info_t chip;
        esp_chip_info(&chip);
        Jim_Obj *dict = Jim_NewListObj(interp, NULL, 0);
        Jim_ListAppendElement(interp, dict, Jim_NewStringObj(interp, "cores", -1));
        Jim_ListAppendElement(interp, dict, Jim_NewIntObj(interp, chip.cores));
        Jim_ListAppendElement(interp, dict, Jim_NewStringObj(interp, "revision", -1));
        Jim_ListAppendElement(interp, dict, Jim_NewIntObj(interp, chip.revision));
        Jim_ListAppendElement(interp, dict, Jim_NewStringObj(interp, "heap_free", -1));
        Jim_ListAppendElement(interp, dict, Jim_NewIntObj(interp, (jim_wide)esp_get_free_heap_size()));
        Jim_SetResult(interp, dict);
        return JIM_OK;
    }
    else if (strcmp(subcmd, "sleep") == 0) {
        /* esp32 sleep <ms> - cooperatively yield via vTaskDelay */
        if (argc != 3) {
            Jim_SetResultString(interp, "wrong # args: should be \"esp32 sleep ms\"", -1);
            return JIM_ERR;
        }
        long ms;
        if (Jim_GetLong(interp, argv[2], &ms) != JIM_OK) {
            return JIM_ERR;
        }
        vTaskDelay(pdMS_TO_TICKS(ms));
        return JIM_OK;
    }

    Jim_SetResultFormatted(interp, "unknown esp32 subcommand \"%s\": should be heap, info, restart, or sleep", subcmd);
    return JIM_ERR;
}

/* ---------------------------------------------------------------------------
 * puts command override for ESP32 UART console
 * ---------------------------------------------------------------------------*/

static int Jim_Esp32PutsCmd(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    const char *str;
    int nonewline = 0;

    if (argc == 2) {
        str = Jim_String(argv[1]);
    }
    else if (argc == 3) {
        /* puts -nonewline str  OR  puts channelId str */
        const char *opt = Jim_String(argv[1]);
        if (strcmp(opt, "-nonewline") == 0) {
            nonewline = 1;
            str = Jim_String(argv[2]);
        }
        else {
            /* Ignore channel id, just print to stdout */
            str = Jim_String(argv[2]);
        }
    }
    else {
        Jim_SetResultString(interp, "wrong # args: should be \"puts ?-nonewline? ?channelId? string\"", -1);
        return JIM_ERR;
    }

    fputs(str, stdout);
    if (!nonewline) {
        fputc('\n', stdout);
    }
    fflush(stdout);
    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * gets command for UART console input
 * ---------------------------------------------------------------------------*/

static int Jim_Esp32GetsCmd(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    char buf[256];

    if (argc > 2) {
        Jim_SetResultString(interp, "wrong # args: should be \"gets ?varName?\"", -1);
        return JIM_ERR;
    }

    if (fgets(buf, sizeof(buf), stdin) == NULL) {
        /* EOF or error */
        if (argc == 2) {
            Jim_SetVariable(interp, argv[1], Jim_NewStringObj(interp, "", 0));
            Jim_SetResultInt(interp, -1);
        } else {
            Jim_SetResultString(interp, "", 0);
        }
        return JIM_OK;
    }

    /* Strip trailing newline */
    int len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') {
        buf[--len] = '\0';
    }
    if (len > 0 && buf[len - 1] == '\r') {
        buf[--len] = '\0';
    }

    if (argc == 2) {
        /* gets varName - store in variable, return length */
        Jim_SetVariable(interp, argv[1], Jim_NewStringObj(interp, buf, len));
        Jim_SetResultInt(interp, len);
    } else {
        /* gets - return the line */
        Jim_SetResultString(interp, buf, len);
    }
    return JIM_OK;
}

/* Forward declaration */
static int Jim_PasteModeCmd(Jim_Interp *interp, int argc, Jim_Obj *const *argv);

/* ---------------------------------------------------------------------------
 * dotsugar command: enable/disable $var.key.subkey dict access
 * ---------------------------------------------------------------------------*/

/* Global flag shared with the jim.c parser */
extern int jim_dotsugar_enabled;

static int Jim_DotSugarCmd(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc == 1) {
        /* Query current state */
        Jim_SetResultInt(interp, interp->dotsugar);
        return JIM_OK;
    }
    if (argc == 2) {
        long val;
        if (Jim_GetLong(interp, argv[1], &val) != JIM_OK) return JIM_ERR;
        interp->dotsugar = (val != 0);
        jim_dotsugar_enabled = (val != 0);
        Jim_SetResultInt(interp, interp->dotsugar);
        return JIM_OK;
    }
    Jim_SetResultString(interp, "wrong # args: should be \"dotsugar ?0|1?\"", -1);
    return JIM_ERR;
}

/* ---------------------------------------------------------------------------
 * Platform init
 * ---------------------------------------------------------------------------*/

int Jim_Esp32PlatformInit(Jim_Interp *interp)
{
    /* Install ESP32 memory allocator */
    Jim_Allocator = JimEsp32Allocator;

    /* Set platform variables */
    Jim_SetVariableStrWithStr(interp, "tcl_platform(os)", "ESP-IDF");
    Jim_SetVariableStrWithStr(interp, "tcl_platform(platform)", "esp32");
    Jim_SetVariableStrWithStr(interp, "tcl_platform(byteOrder)", "littleEndian");

    /* Register platform commands */
    Jim_CreateCommand(interp, "esp32", Jim_Esp32InfoCmd, NULL, NULL);
    Jim_CreateCommand(interp, "puts", Jim_Esp32PutsCmd, NULL, NULL);
    Jim_CreateCommand(interp, "gets", Jim_Esp32GetsCmd, NULL, NULL);
    Jim_CreateCommand(interp, "dotsugar", Jim_DotSugarCmd, NULL, NULL);
    Jim_CreateCommand(interp, "paste", Jim_PasteModeCmd, NULL, NULL);

    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Interactive REPL over UART using linenoise
 *
 * Provides line editing (arrow keys, Home/End), command history,
 * and multi-line Tcl script accumulation.
 * ---------------------------------------------------------------------------*/

/* Paste/smart mode toggle.
 * Start in paste/dumb mode (safe for UART — no ESC[6n cursor queries).
 * Toggle via the 'paste' Tcl command. */
static int repl_dumb_mode = 1;

static void repl_set_mode(int dumb)
{
    repl_dumb_mode = dumb;
    linenoiseSetDumbMode(dumb);
}

static int Jim_PasteModeCmd(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc == 1) {
        /* Toggle */
        repl_set_mode(!repl_dumb_mode);
    } else {
        long val;
        if (Jim_GetLong(interp, argv[1], &val) != JIM_OK) return JIM_ERR;
        repl_set_mode(val != 0);
    }
    printf("[%s mode]\n", repl_dumb_mode ? "paste" : "smart");
    Jim_SetResultString(interp, repl_dumb_mode ? "paste" : "smart", -1);
    return JIM_OK;
}

int Jim_Esp32InteractivePrompt(Jim_Interp *interp)
{
    Jim_Obj *scriptObj = NULL;
    int partial = 0;

    /* Configure linenoise */
    linenoiseSetMultiLine(1);
    linenoiseSetMaxLineLen(512);
    linenoiseHistorySetMaxLen(50);
    linenoiseAllowEmpty(false);

    /* Start in paste/dumb mode — safe default for UART.
     * Smart mode sends ESC[6n cursor queries that eat input characters. */
    linenoiseSetDumbMode(1);

    ESP_LOGI(TAG, "Jim Tcl %d.%d on ESP32 - Interactive Mode",
             JIM_ABI_VERSION / 100, JIM_ABI_VERSION % 100);
    ESP_LOGI(TAG, "Free heap: %lu bytes", (unsigned long)esp_get_free_heap_size());
    printf("Type 'paste' to toggle paste/smart mode\n");

    while (1) {
        const char *prompt = partial ? "> " : "jim> ";
        char *line = linenoise(prompt);

        if (line == NULL) {
            /* EOF or Ctrl-C */
            if (partial) {
                Jim_DecrRefCount(interp, scriptObj);
                scriptObj = NULL;
                partial = 0;
            }
            continue;
        }

        if (!partial) {
            scriptObj = Jim_NewStringObj(interp, line, -1);
            Jim_IncrRefCount(scriptObj);
        } else {
            Jim_AppendString(interp, scriptObj, "\n", 1);
            Jim_AppendString(interp, scriptObj, line, -1);
        }

        if (Jim_ScriptIsComplete(interp, scriptObj, NULL)) {
            /* Add complete command to history */
            linenoiseHistoryAdd(line);

            int retcode = Jim_EvalObj(interp, scriptObj);
            Jim_DecrRefCount(interp, scriptObj);
            scriptObj = NULL;
            partial = 0;

            if (retcode == JIM_EXIT) {
                linenoiseFree(line);
                break;
            }

            /* Print result */
            const char *result = Jim_String(Jim_GetResult(interp));
            if (result[0] != '\0') {
                if (retcode == JIM_ERR) {
                    printf("Error: %s\n", result);
                } else {
                    printf("%s\n", result);
                }
            }
        } else {
            /* Incomplete script — accumulate more input */
            int slen;
            const char *s = Jim_GetString(scriptObj, &slen);
            Jim_Obj *newObj = Jim_NewStringObj(interp, s, slen);
            Jim_IncrRefCount(newObj);
            Jim_DecrRefCount(interp, scriptObj);
            scriptObj = newObj;
            partial = 1;
        }

        linenoiseFree(line);
    }

    return JIM_OK;
}
