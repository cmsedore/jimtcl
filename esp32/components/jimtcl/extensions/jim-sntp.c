/* Jim Tcl SNTP Extension for ESP32
 *
 * Provides Tcl commands for network time synchronization:
 *
 *   sntp start ?-server pool.ntp.org? ?-server2 time.nist.gov?
 *   sntp stop
 *   sntp status
 *   sntp time                    ;# ISO 8601 string
 *   sntp time unix               ;# Unix timestamp
 *   sntp time format <fmt>       ;# custom strftime format
 *   sntp timezone <tz_string>    ;# e.g. "EST5EDT" or "CST-8"
 */

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include "jim.h"
#include "jim-subcmd.h"

#include "esp_sntp.h"
#include "esp_log.h"

static const char *TAG = "jim-sntp";

typedef struct {
    int started;
    char server1[64];
    char server2[64];
} sntp_state_t;

static sntp_state_t sntp_state = { 0 };

static int sntp_cmd_start(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (sntp_state.started) {
        Jim_SetResultString(interp, "already started", -1);
        return JIM_OK;
    }

    const char *server1 = "pool.ntp.org";
    const char *server2 = NULL;

    for (int i = 0; i < argc; i++) {
        const char *opt = Jim_String(argv[i]);
        if (strcmp(opt, "-server") == 0 && i + 1 < argc) {
            server1 = Jim_String(argv[++i]);
        }
        else if (strcmp(opt, "-server2") == 0 && i + 1 < argc) {
            server2 = Jim_String(argv[++i]);
        }
        else {
            Jim_SetResultFormatted(interp, "unknown option \"%s\"", opt);
            return JIM_ERR;
        }
    }

    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, server1);
    if (server2) {
        esp_sntp_setservername(1, server2);
    }

    esp_sntp_init();

    strncpy(sntp_state.server1, server1, sizeof(sntp_state.server1) - 1);
    if (server2) {
        strncpy(sntp_state.server2, server2, sizeof(sntp_state.server2) - 1);
    }
    sntp_state.started = 1;

    ESP_LOGI(TAG, "SNTP started, server: %s", server1);
    return JIM_OK;
}

static int sntp_cmd_stop(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (!sntp_state.started) {
        Jim_SetResultString(interp, "not started", -1);
        return JIM_ERR;
    }

    esp_sntp_stop();
    sntp_state.started = 0;

    ESP_LOGI(TAG, "SNTP stopped");
    return JIM_OK;
}

static int sntp_cmd_status(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (!sntp_state.started) {
        Jim_SetResultString(interp, "not_started", -1);
        return JIM_OK;
    }

    sntp_sync_status_t status = esp_sntp_get_sync_status();
    switch (status) {
        case SNTP_SYNC_STATUS_COMPLETED:
            Jim_SetResultString(interp, "synced", -1);
            break;
        case SNTP_SYNC_STATUS_IN_PROGRESS:
            Jim_SetResultString(interp, "in_progress", -1);
            break;
        case SNTP_SYNC_STATUS_RESET:
        default:
            Jim_SetResultString(interp, "in_progress", -1);
            break;
    }
    return JIM_OK;
}

static int sntp_cmd_time(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    time_t now;
    struct tm timeinfo;

    time(&now);
    localtime_r(&now, &timeinfo);

    if (argc == 0) {
        /* Default: ISO 8601 */
        char buf[64];
        strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &timeinfo);
        Jim_SetResultString(interp, buf, -1);
        return JIM_OK;
    }

    const char *subcmd = Jim_String(argv[0]);

    if (strcmp(subcmd, "unix") == 0) {
        Jim_SetResultInt(interp, (jim_wide)now);
        return JIM_OK;
    }
    else if (strcmp(subcmd, "format") == 0) {
        if (argc < 2) {
            Jim_SetResultString(interp, "usage: sntp time format <fmt>", -1);
            return JIM_ERR;
        }
        const char *fmt = Jim_String(argv[1]);
        char buf[128];
        strftime(buf, sizeof(buf), fmt, &timeinfo);
        Jim_SetResultString(interp, buf, -1);
        return JIM_OK;
    }
    else {
        Jim_SetResultFormatted(interp, "unknown time subcommand \"%s\", expected unix or format", subcmd);
        return JIM_ERR;
    }
}

static int sntp_cmd_timezone(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc < 1) {
        Jim_SetResultString(interp, "usage: sntp timezone <tz_string>", -1);
        return JIM_ERR;
    }

    const char *tz = Jim_String(argv[0]);
    setenv("TZ", tz, 1);
    tzset();

    ESP_LOGI(TAG, "Timezone set to: %s", tz);
    return JIM_OK;
}

static const jim_subcmd_type sntp_command_table[] = {
    {   "start",
        "?-server name? ?-server2 name?",
        sntp_cmd_start,
        0,
        -1,
        /* Description: Start SNTP time synchronization */
    },
    {   "stop",
        NULL,
        sntp_cmd_stop,
        0,
        0,
        /* Description: Stop SNTP */
    },
    {   "status",
        NULL,
        sntp_cmd_status,
        0,
        0,
        /* Description: Return SNTP sync status */
    },
    {   "time",
        "?unix? ?format fmt?",
        sntp_cmd_time,
        0,
        -1,
        /* Description: Return current time */
    },
    {   "timezone",
        "tz_string",
        sntp_cmd_timezone,
        1,
        1,
        /* Description: Set timezone */
    },
    { NULL }
};

int Jim_sntpInit(Jim_Interp *interp)
{
    Jim_PackageProvideCheck(interp, "sntp");
    Jim_RegisterSubCmd(interp, "sntp", sntp_command_table, NULL);
    return JIM_OK;
}
