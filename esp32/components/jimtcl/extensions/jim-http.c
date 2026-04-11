/* Jim Tcl HTTP Client Extension for ESP32
 *
 * Provides synchronous and async HTTP requests:
 *
 *   http get <url> ?-header {name value}...? ?-timeout ms? ?-callback {proc task}?
 *   http post <url> ?-body data? ?-type content-type? ?-header ...? ?-timeout ms? ?-callback ...?
 *   http put <url> ?-body data? ?-type content-type? ?-header ...? ?-timeout ms?
 *   http delete <url> ?-header ...? ?-timeout ms?
 *
 * Sync returns: dict {status <code> body <data>}
 * Async (-callback): returns "pending", delivers {proc status body} to named task.
 */

#include <string.h>
#include <stdlib.h>
#include "jim.h"
#include "jim-subcmd.h"
#include "jim-esp32-task.h"

#ifdef CONFIG_JIM_EXT_JSON
#include "jim-json.h"
#endif

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "jim-http";

/* Max response body we'll accumulate (heap-allocated, grown dynamically) */
#define HTTP_MAX_RESPONSE (64 * 1024)

/* ---------------------------------------------------------------------------
 * Response accumulator — used as event handler user_data
 * ---------------------------------------------------------------------------*/

typedef struct {
    char *buf;
    int len;
    int capacity;
} http_response_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_response_t *resp = (http_response_t *)evt->user_data;
    if (!resp) return ESP_OK;

    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (resp->len + evt->data_len > HTTP_MAX_RESPONSE) break;
            /* Grow buffer if needed */
            while (resp->len + evt->data_len > resp->capacity) {
                int new_cap = resp->capacity * 2;
                if (new_cap > HTTP_MAX_RESPONSE) new_cap = HTTP_MAX_RESPONSE;
                char *new_buf = realloc(resp->buf, new_cap);
                if (!new_buf) break;
                resp->buf = new_buf;
                resp->capacity = new_cap;
            }
            if (resp->len + evt->data_len <= resp->capacity) {
                memcpy(resp->buf + resp->len, evt->data, evt->data_len);
                resp->len += evt->data_len;
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

/* ---------------------------------------------------------------------------
 * Common option parsing for all HTTP methods
 * ---------------------------------------------------------------------------*/

typedef struct {
    const char *url;
    const char *body;
    int body_len;
    const char *content_type;
    long timeout_ms;
    /* Headers: stored as pairs in a flat array */
    const char *header_names[8];
    const char *header_values[8];
    int header_count;
    /* Async callback */
    const char *callback_proc;
    const char *callback_target;
    /* JSON mode */
    int json;
} http_options_t;

static int parse_http_options(Jim_Interp *interp, int argc, Jim_Obj *const *argv,
                              http_options_t *opts)
{
    memset(opts, 0, sizeof(*opts));
    opts->timeout_ms = 10000;

    if (argc < 1) {
        Jim_SetResultString(interp, "missing URL", -1);
        return JIM_ERR;
    }
    opts->url = Jim_String(argv[0]);

    for (int i = 1; i < argc; i++) {
        const char *opt = Jim_String(argv[i]);

        if (strcmp(opt, "-header") == 0 && i + 1 < argc) {
            Jim_Obj *hdr = argv[++i];
            if (Jim_ListLength(interp, hdr) != 2) {
                Jim_SetResultString(interp, "-header requires {name value}", -1);
                return JIM_ERR;
            }
            if (opts->header_count < 8) {
                opts->header_names[opts->header_count] = Jim_String(Jim_ListGetIndex(interp, hdr, 0));
                opts->header_values[opts->header_count] = Jim_String(Jim_ListGetIndex(interp, hdr, 1));
                opts->header_count++;
            }
        }
        else if (strcmp(opt, "-body") == 0 && i + 1 < argc) {
            int len;
            opts->body = Jim_GetString(argv[++i], &len);
            opts->body_len = len;
        }
        else if (strcmp(opt, "-type") == 0 && i + 1 < argc) {
            opts->content_type = Jim_String(argv[++i]);
        }
        else if (strcmp(opt, "-timeout") == 0 && i + 1 < argc) {
            if (Jim_GetLong(interp, argv[++i], &opts->timeout_ms) != JIM_OK) return JIM_ERR;
        }
        else if (strcmp(opt, "-callback") == 0 && i + 1 < argc) {
            Jim_Obj *cbObj = argv[++i];
            if (Jim_ListLength(interp, cbObj) != 2) {
                Jim_SetResultString(interp, "-callback requires {procname target_task}", -1);
                return JIM_ERR;
            }
            opts->callback_proc = Jim_String(Jim_ListGetIndex(interp, cbObj, 0));
            opts->callback_target = Jim_String(Jim_ListGetIndex(interp, cbObj, 1));
        }
        else if (strcmp(opt, "-json") == 0) {
            opts->json = 1;
            if (!opts->content_type) {
                opts->content_type = "application/json";
            }
        }
        else {
            Jim_SetResultFormatted(interp, "unknown option \"%s\"", opt);
            return JIM_ERR;
        }
    }
    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Synchronous HTTP request
 * ---------------------------------------------------------------------------*/

static int http_perform_sync(Jim_Interp *interp, esp_http_client_method_t method,
                             http_options_t *opts)
{
    esp_http_client_config_t config = {
        .url = opts->url,
        .method = method,
        .timeout_ms = (int)opts->timeout_ms,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        Jim_SetResultString(interp, "failed to create HTTP client", -1);
        return JIM_ERR;
    }

    /* Set custom headers */
    for (int i = 0; i < opts->header_count; i++) {
        esp_http_client_set_header(client, opts->header_names[i], opts->header_values[i]);
    }

    /* Set body for POST/PUT */
    if (opts->body && opts->body_len > 0) {
        esp_http_client_set_post_field(client, opts->body, opts->body_len);
    }
    if (opts->content_type) {
        esp_http_client_set_header(client, "Content-Type", opts->content_type);
    }

    /* Open connection and send request */
    esp_err_t err = esp_http_client_open(client, opts->body_len > 0 ? opts->body_len : 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        Jim_SetResultFormatted(interp, "HTTP connect failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    /* Write body if present (for POST/PUT) */
    if (opts->body && opts->body_len > 0) {
        int written = esp_http_client_write(client, opts->body, opts->body_len);
        if (written < 0) {
            esp_http_client_cleanup(client);
            Jim_SetResultString(interp, "HTTP write failed", -1);
            return JIM_ERR;
        }
    }

    /* Fetch response headers */
    int64_t content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    /* Read response body */
    int buf_size = (content_length > 0 && content_length < HTTP_MAX_RESPONSE)
                   ? (int)content_length + 1
                   : 4096;
    char *body = malloc(buf_size);
    int body_len = 0;

    if (body) {
        body_len = esp_http_client_read_response(client, body, buf_size - 1);
        if (body_len >= 0) {
            body[body_len] = '\0';
        } else {
            body[0] = '\0';
            body_len = 0;
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    /* Build result dict: {status <code> body <data>} */
    Jim_Obj *result = Jim_NewListObj(interp, NULL, 0);
    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "status", -1));
    Jim_ListAppendElement(interp, result, Jim_NewIntObj(interp, status));
    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "body", -1));
    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, body ? body : "", body_len));
    Jim_SetResult(interp, result);

    free(body);
    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Async HTTP request — runs in a short-lived FreeRTOS task
 * ---------------------------------------------------------------------------*/

typedef struct {
    esp_http_client_method_t method;
    char *url;
    char *body;
    int body_len;
    char *content_type;
    long timeout_ms;
    char *header_names[8];
    char *header_values[8];
    int header_count;
    char callback_proc[64];
    char callback_target[16];
} http_async_ctx_t;

static void http_async_task(void *param)
{
    http_async_ctx_t *ctx = (http_async_ctx_t *)param;
    int status = 0;
    char *body = NULL;
    int body_len = 0;

    esp_http_client_config_t config = {
        .url = ctx->url,
        .method = ctx->method,
        .timeout_ms = (int)ctx->timeout_ms,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client) {
        for (int i = 0; i < ctx->header_count; i++) {
            esp_http_client_set_header(client, ctx->header_names[i], ctx->header_values[i]);
        }
        if (ctx->body && ctx->body_len > 0) {
            esp_http_client_set_post_field(client, ctx->body, ctx->body_len);
        }
        if (ctx->content_type) {
            esp_http_client_set_header(client, "Content-Type", ctx->content_type);
        }

        if (esp_http_client_open(client, ctx->body_len > 0 ? ctx->body_len : 0) == ESP_OK) {
            if (ctx->body && ctx->body_len > 0) {
                esp_http_client_write(client, ctx->body, ctx->body_len);
            }
            esp_http_client_fetch_headers(client);
            status = esp_http_client_get_status_code(client);

            body = malloc(HTTP_MAX_RESPONSE);
            if (body) {
                body_len = esp_http_client_read_response(client, body, HTTP_MAX_RESPONSE - 1);
                if (body_len >= 0) body[body_len] = '\0';
                else { body[0] = '\0'; body_len = 0; }
            }
            esp_http_client_close(client);
        }
        esp_http_client_cleanup(client);
    }

    /* Deliver result to callback target */
    char *script = malloc((body_len > 0 ? body_len : 0) + 256);
    if (script) {
        snprintf(script, (body_len > 0 ? body_len : 0) + 256, "%s %d {%.*s}",
                 ctx->callback_proc, status, body_len, body ? body : "");
        if (task_send_to_name(ctx->callback_target, script) != 0) {
            ESP_LOGW(TAG, "Async HTTP callback delivery failed to '%s'", ctx->callback_target);
        }
        free(script);
    }

    /* Cleanup */
    free(body);
    free(ctx->url);
    free(ctx->body);
    free(ctx->content_type);
    for (int i = 0; i < ctx->header_count; i++) {
        free(ctx->header_names[i]);
        free(ctx->header_values[i]);
    }
    free(ctx);
    vTaskDelete(NULL);
}

static int http_perform_async(Jim_Interp *interp, esp_http_client_method_t method,
                              http_options_t *opts)
{
    http_async_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        Jim_SetResultString(interp, "out of memory", -1);
        return JIM_ERR;
    }

    ctx->method = method;
    ctx->url = strdup(opts->url);
    ctx->body = opts->body ? strndup(opts->body, opts->body_len) : NULL;
    ctx->body_len = opts->body_len;
    ctx->content_type = opts->content_type ? strdup(opts->content_type) : NULL;
    ctx->timeout_ms = opts->timeout_ms;
    ctx->header_count = opts->header_count;
    for (int i = 0; i < opts->header_count; i++) {
        ctx->header_names[i] = strdup(opts->header_names[i]);
        ctx->header_values[i] = strdup(opts->header_values[i]);
    }
    strncpy(ctx->callback_proc, opts->callback_proc, sizeof(ctx->callback_proc) - 1);
    strncpy(ctx->callback_target, opts->callback_target, sizeof(ctx->callback_target) - 1);

    BaseType_t ret = xTaskCreate(http_async_task, "http_async", 8192, ctx, 5, NULL);
    if (ret != pdPASS) {
        free(ctx->url); free(ctx->body); free(ctx->content_type);
        free(ctx);
        Jim_SetResultString(interp, "failed to create async HTTP task", -1);
        return JIM_ERR;
    }

    Jim_SetResultString(interp, "pending", -1);
    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Tcl command handlers
 * ---------------------------------------------------------------------------*/

static int http_method_cmd(Jim_Interp *interp, int argc, Jim_Obj *const *argv,
                           esp_http_client_method_t method)
{
    http_options_t opts;
    if (parse_http_options(interp, argc, argv, &opts) != JIM_OK) return JIM_ERR;

#ifdef CONFIG_JIM_EXT_JSON
    char *json_body = NULL;
    if (opts.json && opts.body && opts.body_len > 0) {
        /* Treat body as a Jim dict and convert to JSON */
        Jim_Obj *bodyObj = Jim_NewStringObj(interp, opts.body, opts.body_len);
        Jim_IncrRefCount(bodyObj);
        json_body = jim_dict_to_json(interp, bodyObj);
        Jim_DecrRefCount(interp, bodyObj);
        if (!json_body) return JIM_ERR;
        opts.body = json_body;
        opts.body_len = strlen(json_body);
    }
#endif

    int rc;
    if (opts.callback_proc) {
        rc = http_perform_async(interp, method, &opts);
    } else {
        rc = http_perform_sync(interp, method, &opts);
    }

#ifdef CONFIG_JIM_EXT_JSON
    free(json_body);

    /* Parse response body as JSON if -json and sync succeeded */
    if (opts.json && !opts.callback_proc && rc == JIM_OK) {
        /* Result is dict {status N body "..."} - extract body, parse, replace */
        Jim_Obj *result = Jim_GetResult(interp);
        Jim_IncrRefCount(result);
        int rlen = Jim_ListLength(interp, result);
        /* Find "body" key */
        for (int i = 0; i < rlen - 1; i += 2) {
            const char *key = Jim_String(Jim_ListGetIndex(interp, result, i));
            if (strcmp(key, "body") == 0) {
                Jim_Obj *bodyVal = Jim_ListGetIndex(interp, result, i + 1);
                int blen;
                const char *bstr = Jim_GetString(bodyVal, &blen);
                if (blen > 0) {
                    Jim_Obj *parsed = NULL;
                    if (jim_json_to_dict(interp, bstr, blen) == JIM_OK) {
                        parsed = Jim_GetResult(interp);
                        Jim_IncrRefCount(parsed);
                    }
                    /* Rebuild result dict with parsed body */
                    Jim_Obj *newResult = Jim_NewListObj(interp, NULL, 0);
                    for (int j = 0; j < rlen; j += 2) {
                        Jim_ListAppendElement(interp, newResult,
                            Jim_ListGetIndex(interp, result, j));
                        if (j == i && parsed) {
                            Jim_ListAppendElement(interp, newResult, parsed);
                        } else {
                            Jim_ListAppendElement(interp, newResult,
                                Jim_ListGetIndex(interp, result, j + 1));
                        }
                    }
                    Jim_SetResult(interp, newResult);
                    if (parsed) Jim_DecrRefCount(interp, parsed);
                }
                break;
            }
        }
        Jim_DecrRefCount(interp, result);
    }
#endif

    return rc;
}

static int http_cmd_get(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    return http_method_cmd(interp, argc, argv, HTTP_METHOD_GET);
}

static int http_cmd_post(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    return http_method_cmd(interp, argc, argv, HTTP_METHOD_POST);
}

static int http_cmd_put(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    return http_method_cmd(interp, argc, argv, HTTP_METHOD_PUT);
}

static int http_cmd_delete(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    return http_method_cmd(interp, argc, argv, HTTP_METHOD_DELETE);
}

static const jim_subcmd_type http_command_table[] = {
    {   "get",
        "url ?-header {name value}? ?-timeout ms? ?-callback {proc task}?",
        http_cmd_get,
        1,
        -1,
    },
    {   "post",
        "url ?-body data? ?-type content-type? ?-header {name value}? ?-timeout ms? ?-callback {proc task}?",
        http_cmd_post,
        1,
        -1,
    },
    {   "put",
        "url ?-body data? ?-type content-type? ?-header {name value}? ?-timeout ms?",
        http_cmd_put,
        1,
        -1,
    },
    {   "delete",
        "url ?-header {name value}? ?-timeout ms?",
        http_cmd_delete,
        1,
        -1,
    },
    { NULL }
};

int Jim_httpInit(Jim_Interp *interp)
{
    Jim_PackageProvideCheck(interp, "http");
    Jim_RegisterSubCmd(interp, "http", http_command_table, NULL);
    return JIM_OK;
}
