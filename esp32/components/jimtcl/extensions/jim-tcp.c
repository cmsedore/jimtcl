/* Jim Tcl TCP Extension for ESP32
 *
 * Provides Tcl commands for TCP socket communication using lwIP:
 *
 *   tcp connect <host> <port> ?-mode mpack?
 *   tcp listen <port> ?-mode mpack? ?-callback {proc task}?
 *   tcp send <handle> <data>
 *   tcp receive <handle> ?timeout_ms?
 *   tcp close <handle>
 *   tcp status <handle>
 *   tcp accept <listen_handle> ?timeout_ms?
 */

#include <string.h>
#include <stdlib.h>
#include "jim.h"
#include "jim-subcmd.h"
#include "jim-esp32-task.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef CONFIG_JIM_EXT_MPACK
#include "jim-mpack.h"
#include "cobs.h"
#endif

static const char *TAG = "jim-tcp";

/* ---------------------------------------------------------------------------
 * Static state -- track up to 8 socket handles
 * ---------------------------------------------------------------------------*/

#define TCP_MAX_HANDLES   8
#define TCP_RECV_BUF      1024

typedef enum {
    TCP_MODE_RAW,
    TCP_MODE_MPACK,
} tcp_mode_t;

typedef struct {
    int in_use;
    int fd;
    tcp_mode_t mode;
    int connected;
    int is_listener;
    /* Listener background task state */
    TaskHandle_t listener_task;
    volatile int listener_stop;
    char listener_proc[64];
    char listener_target[16];
} tcp_handle_t;

static tcp_handle_t tcp_handles[TCP_MAX_HANDLES] = { {0} };

/* ---------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------------*/

static int tcp_alloc_handle(void)
{
    for (int i = 0; i < TCP_MAX_HANDLES; i++) {
        if (!tcp_handles[i].in_use) return i;
    }
    return -1;
}

static int tcp_get_handle(Jim_Interp *interp, Jim_Obj *obj, int *idx)
{
    long val;
    if (Jim_GetLong(interp, obj, &val) != JIM_OK) return JIM_ERR;
    if (val < 0 || val >= TCP_MAX_HANDLES || !tcp_handles[val].in_use) {
        Jim_SetResultFormatted(interp, "invalid tcp handle: %ld", val);
        return JIM_ERR;
    }
    *idx = (int)val;
    return JIM_OK;
}

static void tcp_free_handle(int idx)
{
    tcp_handle_t *h = &tcp_handles[idx];
    if (h->fd >= 0) {
        close(h->fd);
    }
    memset(h, 0, sizeof(*h));
    h->fd = -1;
}

/* ---------------------------------------------------------------------------
 * Listener background task
 * ---------------------------------------------------------------------------*/

typedef struct {
    int handle_idx;
} tcp_listener_ctx_t;

#ifdef CONFIG_JIM_EXT_MPACK
/* Deliver an mpack-decoded message from a client fd to the target task */
static void tcp_listener_deliver_mpack(tcp_handle_t *lh, int client_fd)
{
    uint8_t buf[TCP_RECV_BUF];
    uint8_t accum[TCP_RECV_BUF * 2];
    int accum_len = 0;

    while (!lh->listener_stop) {
        int n = recv(client_fd, buf, sizeof(buf), 0);
        if (n <= 0) break;

        for (int i = 0; i < n; i++) {
            if (buf[i] == 0x00) {
                /* End of COBS frame -- decode */
                if (accum_len > 0) {
                    uint8_t decoded[TCP_RECV_BUF * 2];
                    size_t dec_len = cobs_decode(accum, accum_len, decoded, sizeof(decoded));
                    if (dec_len > 0) {
                        /* Build script: {proc} <mpack_dict> */
                        /* We need a temporary interp to convert mpack to string.
                         * Instead, deliver as hex-encoded mpack for the target
                         * task to decode, or use a simpler approach:
                         * Deliver as binary data in a brace-safe format. */
                        /* For listener delivery, format as hex string that the
                         * target proc can decode with [mpack decode] */
                        size_t script_len = strlen(lh->listener_proc) + dec_len * 2 + 32;
                        char *script = malloc(script_len);
                        if (!script) continue;

                        int off = snprintf(script, script_len, "%s ", lh->listener_proc);
                        /* Encode binary data as hex for safe Tcl transport */
                        for (size_t j = 0; j < dec_len; j++) {
                            off += snprintf(script + off, script_len - off, "%02x", decoded[j]);
                        }

                        if (task_send_to_name(lh->listener_target, script) != 0) {
                            ESP_LOGW(TAG, "TCP listener delivery failed -> task '%s'",
                                     lh->listener_target);
                        }
                        free(script);
                    }
                    accum_len = 0;
                }
            } else {
                if (accum_len < (int)sizeof(accum)) {
                    accum[accum_len++] = buf[i];
                } else {
                    /* Overflow -- discard frame */
                    accum_len = 0;
                }
            }
        }
    }
}
#endif /* CONFIG_JIM_EXT_MPACK */

static void tcp_listener_deliver_raw(tcp_handle_t *lh, int client_fd)
{
    uint8_t buf[TCP_RECV_BUF];

    while (!lh->listener_stop) {
        int n = recv(client_fd, buf, sizeof(buf), 0);
        if (n <= 0) break;

        /* Build: {proc} {data} */
        size_t script_len = strlen(lh->listener_proc) + n * 4 + 16;
        char *script = malloc(script_len);
        if (!script) continue;

        int off = snprintf(script, script_len, "%s {", lh->listener_proc);
        for (int i = 0; i < n; i++) {
            if (buf[i] >= 0x20 && buf[i] < 0x7f &&
                buf[i] != '{' && buf[i] != '}' && buf[i] != '\\') {
                script[off++] = (char)buf[i];
            } else {
                off += snprintf(script + off, 8, "\\x%02x", buf[i]);
            }
        }
        script[off++] = '}';
        script[off] = '\0';

        if (task_send_to_name(lh->listener_target, script) != 0) {
            ESP_LOGW(TAG, "TCP listener delivery failed -> task '%s'",
                     lh->listener_target);
        }
        free(script);
    }
}

static void tcp_listener_fn(void *param)
{
    tcp_listener_ctx_t *ctx = (tcp_listener_ctx_t *)param;
    int hidx = ctx->handle_idx;
    free(ctx);

    tcp_handle_t *lh = &tcp_handles[hidx];

    while (!lh->listener_stop) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(lh->fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (!lh->listener_stop) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            continue;
        }

        ESP_LOGI(TAG, "TCP listener accepted client fd=%d", client_fd);

#ifdef CONFIG_JIM_EXT_MPACK
        if (lh->mode == TCP_MODE_MPACK) {
            tcp_listener_deliver_mpack(lh, client_fd);
        } else
#endif
        {
            tcp_listener_deliver_raw(lh, client_fd);
        }

        close(client_fd);
    }

    ESP_LOGI(TAG, "TCP listener stopped on handle %d", hidx);
    lh->listener_task = NULL;
    vTaskDelete(NULL);
}

/* ---------------------------------------------------------------------------
 * Subcommand: tcp connect <host> <port> ?-mode mpack?
 * ---------------------------------------------------------------------------*/

static int tcp_cmd_connect(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc < 2) {
        Jim_SetResultString(interp,
            "wrong # args: should be \"tcp connect host port ?-mode mpack?\"", -1);
        return JIM_ERR;
    }

    const char *host = Jim_String(argv[0]);
    long port;
    if (Jim_GetLong(interp, argv[1], &port) != JIM_OK) return JIM_ERR;

    tcp_mode_t mode = TCP_MODE_RAW;
    for (int i = 2; i < argc; i++) {
        const char *opt = Jim_String(argv[i]);
        if (strcmp(opt, "-mode") == 0 && i + 1 < argc) {
            const char *mstr = Jim_String(argv[++i]);
            if (strcmp(mstr, "mpack") == 0) {
#ifdef CONFIG_JIM_EXT_MPACK
                mode = TCP_MODE_MPACK;
#else
                Jim_SetResultString(interp, "mpack support not compiled in", -1);
                return JIM_ERR;
#endif
            } else if (strcmp(mstr, "raw") == 0) {
                mode = TCP_MODE_RAW;
            } else {
                Jim_SetResultFormatted(interp, "unknown mode \"%s\": should be raw or mpack", mstr);
                return JIM_ERR;
            }
        } else {
            Jim_SetResultFormatted(interp, "unknown option \"%s\"", opt);
            return JIM_ERR;
        }
    }

    int hidx = tcp_alloc_handle();
    if (hidx < 0) {
        Jim_SetResultString(interp, "no free tcp handles (max 8)", -1);
        return JIM_ERR;
    }

    /* Resolve host */
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%ld", port);

    struct addrinfo hints = {0};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    int err = getaddrinfo(host, port_str, &hints, &res);
    if (err != 0 || res == NULL) {
        Jim_SetResultFormatted(interp, "getaddrinfo failed for %s:%ld", host, port);
        return JIM_ERR;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        Jim_SetResultString(interp, "socket() failed", -1);
        return JIM_ERR;
    }

    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        freeaddrinfo(res);
        close(fd);
        Jim_SetResultFormatted(interp, "connect failed to %s:%ld", host, port);
        return JIM_ERR;
    }
    freeaddrinfo(res);

    tcp_handle_t *h = &tcp_handles[hidx];
    h->in_use = 1;
    h->fd = fd;
    h->mode = mode;
    h->connected = 1;
    h->is_listener = 0;
    h->listener_task = NULL;
    h->listener_stop = 0;

    ESP_LOGI(TAG, "TCP connected to %s:%ld -> handle %d (fd=%d, mode=%s)",
             host, port, hidx, fd, mode == TCP_MODE_MPACK ? "mpack" : "raw");

    Jim_SetResultInt(interp, hidx);
    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Subcommand: tcp listen <port> ?-mode mpack? ?-callback {proc task}?
 * ---------------------------------------------------------------------------*/

static int tcp_cmd_listen(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc < 1) {
        Jim_SetResultString(interp,
            "wrong # args: should be \"tcp listen port ?-mode mpack? ?-callback {proc task}?\"", -1);
        return JIM_ERR;
    }

    long port;
    if (Jim_GetLong(interp, argv[0], &port) != JIM_OK) return JIM_ERR;

    tcp_mode_t mode = TCP_MODE_RAW;
    int has_callback = 0;
    const char *cb_proc = NULL;
    const char *cb_target = NULL;

    for (int i = 1; i < argc; i++) {
        const char *opt = Jim_String(argv[i]);
        if (strcmp(opt, "-mode") == 0 && i + 1 < argc) {
            const char *mstr = Jim_String(argv[++i]);
            if (strcmp(mstr, "mpack") == 0) {
#ifdef CONFIG_JIM_EXT_MPACK
                mode = TCP_MODE_MPACK;
#else
                Jim_SetResultString(interp, "mpack support not compiled in", -1);
                return JIM_ERR;
#endif
            } else if (strcmp(mstr, "raw") == 0) {
                mode = TCP_MODE_RAW;
            } else {
                Jim_SetResultFormatted(interp, "unknown mode \"%s\"", mstr);
                return JIM_ERR;
            }
        } else if (strcmp(opt, "-callback") == 0 && i + 1 < argc) {
            Jim_Obj *cbObj = argv[++i];
            if (Jim_ListLength(interp, cbObj) != 2) {
                Jim_SetResultString(interp, "-callback requires {procname target_task}", -1);
                return JIM_ERR;
            }
            cb_proc = Jim_String(Jim_ListGetIndex(interp, cbObj, 0));
            cb_target = Jim_String(Jim_ListGetIndex(interp, cbObj, 1));
            has_callback = 1;
        } else {
            Jim_SetResultFormatted(interp, "unknown option \"%s\"", opt);
            return JIM_ERR;
        }
    }

    int hidx = tcp_alloc_handle();
    if (hidx < 0) {
        Jim_SetResultString(interp, "no free tcp handles (max 8)", -1);
        return JIM_ERR;
    }

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        Jim_SetResultString(interp, "socket() failed", -1);
        return JIM_ERR;
    }

    int opt_val = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt_val, sizeof(opt_val));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        Jim_SetResultFormatted(interp, "bind failed on port %ld", port);
        return JIM_ERR;
    }

    if (listen(fd, 4) != 0) {
        close(fd);
        Jim_SetResultString(interp, "listen() failed", -1);
        return JIM_ERR;
    }

    tcp_handle_t *h = &tcp_handles[hidx];
    h->in_use = 1;
    h->fd = fd;
    h->mode = mode;
    h->connected = 0;
    h->is_listener = 1;
    h->listener_task = NULL;
    h->listener_stop = 0;

    ESP_LOGI(TAG, "TCP listening on port %ld -> handle %d (fd=%d, mode=%s)",
             port, hidx, fd, mode == TCP_MODE_MPACK ? "mpack" : "raw");

    /* Start background listener task if callback provided */
    if (has_callback) {
        strncpy(h->listener_proc, cb_proc, sizeof(h->listener_proc) - 1);
        h->listener_proc[sizeof(h->listener_proc) - 1] = '\0';
        strncpy(h->listener_target, cb_target, sizeof(h->listener_target) - 1);
        h->listener_target[sizeof(h->listener_target) - 1] = '\0';

        tcp_listener_ctx_t *ctx = malloc(sizeof(*ctx));
        if (!ctx) {
            tcp_free_handle(hidx);
            Jim_SetResultString(interp, "out of memory", -1);
            return JIM_ERR;
        }
        ctx->handle_idx = hidx;

        char task_name[24];
        snprintf(task_name, sizeof(task_name), "tcp_listen%d", hidx);

        BaseType_t ret = xTaskCreate(tcp_listener_fn, task_name, 4096, ctx, 6,
                                     &h->listener_task);
        if (ret != pdPASS) {
            free(ctx);
            tcp_free_handle(hidx);
            Jim_SetResultString(interp, "failed to create listener task", -1);
            return JIM_ERR;
        }

        ESP_LOGI(TAG, "TCP listener task started: %s -> task '%s'",
                 cb_proc, cb_target);
    }

    Jim_SetResultInt(interp, hidx);
    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Subcommand: tcp send <handle> <data>
 * ---------------------------------------------------------------------------*/

static int tcp_cmd_send(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc < 2) {
        Jim_SetResultString(interp,
            "wrong # args: should be \"tcp send handle data\"", -1);
        return JIM_ERR;
    }

    int hidx;
    if (tcp_get_handle(interp, argv[0], &hidx) != JIM_OK) return JIM_ERR;

    tcp_handle_t *h = &tcp_handles[hidx];
    if (!h->connected && !h->is_listener) {
        Jim_SetResultString(interp, "handle not connected", -1);
        return JIM_ERR;
    }

#ifdef CONFIG_JIM_EXT_MPACK
    if (h->mode == TCP_MODE_MPACK) {
        /* Encode dict as mpack, COBS-frame, send with 0x00 delimiter */
        size_t mpack_len = 0;
        uint8_t *mpack_data = jim_dict_to_mpack(interp, argv[1], &mpack_len);
        if (!mpack_data) return JIM_ERR;

        size_t cobs_max = COBS_MAX_ENCODED_SIZE(mpack_len);
        uint8_t *cobs_buf = malloc(cobs_max + 1); /* +1 for delimiter */
        if (!cobs_buf) {
            free(mpack_data);
            Jim_SetResultString(interp, "out of memory", -1);
            return JIM_ERR;
        }

        size_t cobs_len = cobs_encode(mpack_data, mpack_len, cobs_buf, cobs_max);
        free(mpack_data);

        cobs_buf[cobs_len] = 0x00; /* COBS frame delimiter */

        int sent = send(h->fd, cobs_buf, cobs_len + 1, 0);
        free(cobs_buf);

        if (sent < 0) {
            Jim_SetResultString(interp, "send failed", -1);
            return JIM_ERR;
        }

        Jim_SetResultInt(interp, sent);
        return JIM_OK;
    }
#endif /* CONFIG_JIM_EXT_MPACK */

    /* Raw mode */
    int data_len;
    const char *data = Jim_GetString(argv[1], &data_len);

    int sent = send(h->fd, data, data_len, 0);
    if (sent < 0) {
        Jim_SetResultString(interp, "send failed", -1);
        return JIM_ERR;
    }

    Jim_SetResultInt(interp, sent);
    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Subcommand: tcp receive <handle> ?timeout_ms?
 * ---------------------------------------------------------------------------*/

static int tcp_cmd_receive(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc < 1) {
        Jim_SetResultString(interp,
            "wrong # args: should be \"tcp receive handle ?timeout_ms?\"", -1);
        return JIM_ERR;
    }

    int hidx;
    if (tcp_get_handle(interp, argv[0], &hidx) != JIM_OK) return JIM_ERR;

    tcp_handle_t *h = &tcp_handles[hidx];
    if (!h->connected) {
        Jim_SetResultString(interp, "handle not connected", -1);
        return JIM_ERR;
    }

    long timeout_ms = 5000;
    if (argc >= 2) {
        if (Jim_GetLong(interp, argv[1], &timeout_ms) != JIM_OK) return JIM_ERR;
    }

    /* Set receive timeout */
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(h->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

#ifdef CONFIG_JIM_EXT_MPACK
    if (h->mode == TCP_MODE_MPACK) {
        /* Accumulate until 0x00 delimiter */
        uint8_t accum[TCP_RECV_BUF * 2];
        int accum_len = 0;
        uint8_t byte;

        while (accum_len < (int)sizeof(accum)) {
            int n = recv(h->fd, &byte, 1, 0);
            if (n <= 0) {
                if (accum_len == 0) {
                    Jim_SetResultString(interp, "", 0);
                    return JIM_OK;
                }
                break;
            }
            if (byte == 0x00) {
                /* End of frame */
                uint8_t decoded[TCP_RECV_BUF * 2];
                size_t dec_len = cobs_decode(accum, accum_len, decoded, sizeof(decoded));
                if (dec_len == 0) {
                    Jim_SetResultString(interp, "COBS decode error", -1);
                    return JIM_ERR;
                }
                return jim_mpack_to_dict(interp, decoded, dec_len);
            }
            accum[accum_len++] = byte;
        }

        Jim_SetResultString(interp, "mpack frame too large or incomplete", -1);
        return JIM_ERR;
    }
#endif /* CONFIG_JIM_EXT_MPACK */

    /* Raw mode */
    uint8_t buf[TCP_RECV_BUF];
    int n = recv(h->fd, buf, sizeof(buf), 0);
    if (n < 0) {
        Jim_SetResultString(interp, "", 0);
        return JIM_OK;
    }

    Jim_SetResultString(interp, (const char *)buf, n);
    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Subcommand: tcp close <handle>
 * ---------------------------------------------------------------------------*/

static int tcp_cmd_close(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc < 1) {
        Jim_SetResultString(interp,
            "wrong # args: should be \"tcp close handle\"", -1);
        return JIM_ERR;
    }

    int hidx;
    if (tcp_get_handle(interp, argv[0], &hidx) != JIM_OK) return JIM_ERR;

    tcp_handle_t *h = &tcp_handles[hidx];

    /* Stop listener task if running */
    if (h->listener_task) {
        h->listener_stop = 1;
        int wait = 0;
        while (h->listener_task && wait < 20) {
            vTaskDelay(pdMS_TO_TICKS(50));
            wait++;
        }
        if (h->listener_task) {
            ESP_LOGW(TAG, "TCP listener on handle %d did not stop cleanly", hidx);
            vTaskDelete(h->listener_task);
            h->listener_task = NULL;
        }
        h->listener_stop = 0;
    }

    ESP_LOGI(TAG, "TCP handle %d closed (fd=%d)", hidx, h->fd);
    tcp_free_handle(hidx);
    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Subcommand: tcp status <handle>
 * ---------------------------------------------------------------------------*/

static int tcp_cmd_status(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc < 1) {
        Jim_SetResultString(interp,
            "wrong # args: should be \"tcp status handle\"", -1);
        return JIM_ERR;
    }

    int hidx;
    if (tcp_get_handle(interp, argv[0], &hidx) != JIM_OK) return JIM_ERR;

    tcp_handle_t *h = &tcp_handles[hidx];

    if (h->is_listener) {
        Jim_SetResultString(interp, "listening", -1);
    } else if (h->connected) {
        /* Check if still connected with a zero-length recv */
        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(h->fd, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err != 0) {
            h->connected = 0;
            Jim_SetResultString(interp, "closed", -1);
        } else {
            Jim_SetResultString(interp, "connected", -1);
        }
    } else {
        Jim_SetResultString(interp, "closed", -1);
    }

    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Subcommand: tcp accept <listen_handle> ?timeout_ms?
 * ---------------------------------------------------------------------------*/

static int tcp_cmd_accept(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc < 1) {
        Jim_SetResultString(interp,
            "wrong # args: should be \"tcp accept listen_handle ?timeout_ms?\"", -1);
        return JIM_ERR;
    }

    int hidx;
    if (tcp_get_handle(interp, argv[0], &hidx) != JIM_OK) return JIM_ERR;

    tcp_handle_t *h = &tcp_handles[hidx];
    if (!h->is_listener) {
        Jim_SetResultString(interp, "handle is not a listener", -1);
        return JIM_ERR;
    }

    long timeout_ms = 5000;
    if (argc >= 2) {
        if (Jim_GetLong(interp, argv[1], &timeout_ms) != JIM_OK) return JIM_ERR;
    }

    /* Set accept timeout */
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(h->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int client_fd = accept(h->fd, (struct sockaddr *)&client_addr, &addr_len);
    if (client_fd < 0) {
        Jim_SetResultString(interp, "accept timed out or failed", -1);
        return JIM_ERR;
    }

    /* Allocate a new handle for the accepted connection */
    int new_hidx = tcp_alloc_handle();
    if (new_hidx < 0) {
        close(client_fd);
        Jim_SetResultString(interp, "no free tcp handles (max 8)", -1);
        return JIM_ERR;
    }

    tcp_handle_t *nh = &tcp_handles[new_hidx];
    nh->in_use = 1;
    nh->fd = client_fd;
    nh->mode = h->mode; /* Inherit mode from listener */
    nh->connected = 1;
    nh->is_listener = 0;
    nh->listener_task = NULL;
    nh->listener_stop = 0;

    ESP_LOGI(TAG, "TCP accepted client -> handle %d (fd=%d)", new_hidx, client_fd);

    Jim_SetResultInt(interp, new_hidx);
    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Subcommand table and init
 * ---------------------------------------------------------------------------*/

static const jim_subcmd_type tcp_command_table[] = {
    {   "connect",
        "host port ?-mode mpack?",
        tcp_cmd_connect,
        2,
        -1,
        /* Description: Connect to a TCP server */
    },
    {   "listen",
        "port ?-mode mpack? ?-callback {proc task}?",
        tcp_cmd_listen,
        1,
        -1,
        /* Description: Listen for TCP connections */
    },
    {   "send",
        "handle data",
        tcp_cmd_send,
        2,
        2,
        /* Description: Send data on a TCP connection */
    },
    {   "receive",
        "handle ?timeout_ms?",
        tcp_cmd_receive,
        1,
        2,
        /* Description: Receive data from a TCP connection */
    },
    {   "close",
        "handle",
        tcp_cmd_close,
        1,
        1,
        /* Description: Close a TCP connection */
    },
    {   "status",
        "handle",
        tcp_cmd_status,
        1,
        1,
        /* Description: Get connection status */
    },
    {   "accept",
        "listen_handle ?timeout_ms?",
        tcp_cmd_accept,
        1,
        2,
        /* Description: Accept an incoming TCP connection */
    },
    { NULL }
};

int Jim_tcpInit(Jim_Interp *interp)
{
    /* Initialize handle slots */
    for (int i = 0; i < TCP_MAX_HANDLES; i++) {
        tcp_handles[i].fd = -1;
    }

    Jim_PackageProvideCheck(interp, "tcp");
    Jim_RegisterSubCmd(interp, "tcp", tcp_command_table, NULL);
    return JIM_OK;
}
