/* Jim Tcl Syslog Extension for ESP32
 *
 * UDP syslog client (simplified RFC 5424) using lwIP sockets.
 *
 *   syslog open <host> ?-port 514?
 *   syslog send <message> ?-facility 1? ?-severity 6?
 *   syslog close
 *   syslog redirect ?0|1?
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "jim.h"
#include "jim-subcmd.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_log.h"

static const char *TAG = "jim-syslog";

/* -----------------------------------------------------------------------
 * Static syslog state
 * ----------------------------------------------------------------------- */

static int syslog_fd = -1;
static struct sockaddr_in syslog_addr;
static int syslog_redirect_enabled = 0;

/* -----------------------------------------------------------------------
 * Internal: send a raw syslog UDP message
 * ----------------------------------------------------------------------- */

static int syslog_send_msg(const char *msg, int facility, int severity)
{
    if (syslog_fd < 0) return -1;

    int pri = facility * 8 + severity;

    /* Format: <PRI>1 - - - - - - MESSAGE */
    char buf[1024];
    int len = snprintf(buf, sizeof(buf), "<%d>1 - - - - - - %s", pri, msg);
    if (len < 0 || len >= (int)sizeof(buf)) {
        len = sizeof(buf) - 1;
    }

    int sent = sendto(syslog_fd, buf, len, 0,
                      (struct sockaddr *)&syslog_addr, sizeof(syslog_addr));
    return (sent > 0) ? 0 : -1;
}

/* -----------------------------------------------------------------------
 * Replacement "puts" command that also sends to syslog
 *
 * When syslog redirect is enabled, output goes to both UART (via the
 * original puts) and syslog.
 * ----------------------------------------------------------------------- */

static Jim_Obj *original_puts_cmd = NULL;

static int syslog_puts_cmd(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    /* Gather the output string (last argument, same as standard puts) */
    const char *str = NULL;
    int nonewline = 0;

    if (argc == 2) {
        str = Jim_String(argv[1]);
    } else if (argc == 3) {
        const char *opt = Jim_String(argv[1]);
        if (strcmp(opt, "-nonewline") == 0) {
            nonewline = 1;
            str = Jim_String(argv[2]);
        } else {
            /* Channel + string: argv[1]=channel, argv[2]=string */
            str = Jim_String(argv[2]);
        }
    } else if (argc == 4) {
        /* -nonewline channel string */
        nonewline = 1;
        str = Jim_String(argv[3]);
    }

    /* Send to syslog if we have a message and redirect is on */
    if (str && syslog_redirect_enabled && syslog_fd >= 0) {
        syslog_send_msg(str, 1, 6);  /* facility=user, severity=info */
    }

    /* Chain to original puts */
    if (original_puts_cmd) {
        return Jim_EvalObjPrefix(interp, original_puts_cmd, argc - 1, argv + 1);
    }

    /* Fallback: print to stdout directly */
    if (str) {
        fputs(str, stdout);
        if (!nonewline) fputc('\n', stdout);
    }

    return JIM_OK;
}

/* -----------------------------------------------------------------------
 * Tcl subcommands
 * ----------------------------------------------------------------------- */

static int syslog_cmd_open(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    const char *host = Jim_String(argv[0]);
    long port = 514;

    /* Parse optional -port */
    for (int i = 1; i < argc; i++) {
        const char *opt = Jim_String(argv[i]);
        if (strcmp(opt, "-port") == 0 && i + 1 < argc) {
            if (Jim_GetLong(interp, argv[++i], &port) != JIM_OK) {
                return JIM_ERR;
            }
        } else {
            Jim_SetResultFormatted(interp, "unknown option \"%s\"", opt);
            return JIM_ERR;
        }
    }

    if (port <= 0 || port > 65535) {
        Jim_SetResultString(interp, "port must be 1-65535", -1);
        return JIM_ERR;
    }

    /* Close existing connection if open */
    if (syslog_fd >= 0) {
        close(syslog_fd);
        syslog_fd = -1;
    }

    /* Resolve hostname */
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%ld", port);

    int err = getaddrinfo(host, port_str, &hints, &res);
    if (err != 0 || res == NULL) {
        Jim_SetResultFormatted(interp, "failed to resolve host \"%s\"", host);
        if (res) freeaddrinfo(res);
        return JIM_ERR;
    }

    /* Copy resolved address */
    memcpy(&syslog_addr, res->ai_addr, sizeof(syslog_addr));
    freeaddrinfo(res);

    /* Create UDP socket */
    syslog_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (syslog_fd < 0) {
        Jim_SetResultString(interp, "failed to create UDP socket", -1);
        return JIM_ERR;
    }

    ESP_LOGI(TAG, "syslog opened: %s:%ld (fd=%d)", host, port, syslog_fd);
    return JIM_OK;
}

static int syslog_cmd_send(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (syslog_fd < 0) {
        Jim_SetResultString(interp, "syslog not open", -1);
        return JIM_ERR;
    }

    const char *message = Jim_String(argv[0]);
    long facility = 1;  /* user */
    long severity = 6;  /* informational */

    for (int i = 1; i < argc; i++) {
        const char *opt = Jim_String(argv[i]);
        if (strcmp(opt, "-facility") == 0 && i + 1 < argc) {
            if (Jim_GetLong(interp, argv[++i], &facility) != JIM_OK) {
                return JIM_ERR;
            }
        } else if (strcmp(opt, "-severity") == 0 && i + 1 < argc) {
            if (Jim_GetLong(interp, argv[++i], &severity) != JIM_OK) {
                return JIM_ERR;
            }
        } else {
            Jim_SetResultFormatted(interp, "unknown option \"%s\"", opt);
            return JIM_ERR;
        }
    }

    if (facility < 0 || facility > 23) {
        Jim_SetResultString(interp, "facility must be 0-23", -1);
        return JIM_ERR;
    }
    if (severity < 0 || severity > 7) {
        Jim_SetResultString(interp, "severity must be 0-7", -1);
        return JIM_ERR;
    }

    if (syslog_send_msg(message, (int)facility, (int)severity) != 0) {
        Jim_SetResultString(interp, "syslog sendto failed", -1);
        return JIM_ERR;
    }

    return JIM_OK;
}

static int syslog_cmd_close(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (syslog_fd >= 0) {
        close(syslog_fd);
        ESP_LOGI(TAG, "syslog closed (fd=%d)", syslog_fd);
        syslog_fd = -1;
    }

    syslog_redirect_enabled = 0;
    return JIM_OK;
}

static int syslog_cmd_redirect(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc == 0) {
        /* Query current state */
        Jim_SetResultInt(interp, syslog_redirect_enabled);
        return JIM_OK;
    }

    long enable;
    if (Jim_GetLong(interp, argv[0], &enable) != JIM_OK) {
        return JIM_ERR;
    }

    if (enable && syslog_fd < 0) {
        Jim_SetResultString(interp, "syslog not open; call 'syslog open' first", -1);
        return JIM_ERR;
    }

    if (enable && !syslog_redirect_enabled) {
        /* Save original puts and install our wrapper */
        Jim_Cmd *cmd = Jim_GetCommand(interp, Jim_NewStringObj(interp, "puts", -1), JIM_NONE);
        if (cmd) {
            original_puts_cmd = Jim_NewStringObj(interp, "_syslog_orig_puts", -1);
            Jim_IncrRefCount(original_puts_cmd);

            /* Rename the original puts to _syslog_orig_puts */
            Jim_RenameCommand(interp, "puts", "_syslog_orig_puts");

            /* Create our replacement puts */
            Jim_CreateCommand(interp, "puts", syslog_puts_cmd, NULL, NULL);
        }

        syslog_redirect_enabled = 1;
        ESP_LOGI(TAG, "syslog redirect enabled");
    } else if (!enable && syslog_redirect_enabled) {
        /* Restore original puts */
        Jim_DeleteCommand(interp, "puts");
        Jim_RenameCommand(interp, "_syslog_orig_puts", "puts");

        if (original_puts_cmd) {
            Jim_DecrRefCount(interp, original_puts_cmd);
            original_puts_cmd = NULL;
        }

        syslog_redirect_enabled = 0;
        ESP_LOGI(TAG, "syslog redirect disabled");
    }

    Jim_SetResultInt(interp, syslog_redirect_enabled);
    return JIM_OK;
}

/* -----------------------------------------------------------------------
 * Subcommand dispatch table
 * ----------------------------------------------------------------------- */

static const jim_subcmd_type syslog_command_table[] = {
    {   "open",
        "host ?-port 514?",
        syslog_cmd_open,
        1,
        -1,
        /* Description: Open UDP syslog connection to host */
    },
    {   "send",
        "message ?-facility 1? ?-severity 6?",
        syslog_cmd_send,
        1,
        -1,
        /* Description: Send a syslog message */
    },
    {   "close",
        "",
        syslog_cmd_close,
        0,
        0,
        /* Description: Close the syslog connection */
    },
    {   "redirect",
        "?0|1?",
        syslog_cmd_redirect,
        0,
        1,
        /* Description: Enable/disable puts output redirection to syslog */
    },
    { NULL }
};

int Jim_syslogInit(Jim_Interp *interp)
{
    Jim_PackageProvideCheck(interp, "syslog");
    Jim_RegisterSubCmd(interp, "syslog", syslog_command_table, NULL);
    return JIM_OK;
}
