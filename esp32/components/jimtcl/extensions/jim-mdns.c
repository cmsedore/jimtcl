/* Jim Tcl mDNS Extension for ESP32
 *
 * Provides Tcl commands for mDNS zero-config networking:
 *
 *   mdns init ?-hostname name? ?-instance name?
 *   mdns deinit
 *   mdns service add <type> <proto> <port> ?-txt {key1 val1 key2 val2}?
 *   mdns service remove <type> <proto>
 *   mdns query <service_type> <proto> ?-timeout ms?
 *   mdns resolve <hostname> ?-timeout ms?
 *   mdns hostname ?name?
 *   mdns status
 */

#include <string.h>
#include <stdlib.h>
#include "jim.h"
#include "jim-subcmd.h"

#include "mdns.h"
#include "esp_log.h"

static const char *TAG = "jim-mdns";

typedef struct {
    int initialized;
    char hostname[64];
    char instance[64];
} mdns_state_t;

static mdns_state_t mdns_state = { 0 };

static int mdns_cmd_init(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (mdns_state.initialized) {
        return JIM_OK;
    }

    const char *hostname = NULL;
    const char *instance = NULL;

    for (int i = 0; i < argc; i++) {
        const char *opt = Jim_String(argv[i]);
        if (strcmp(opt, "-hostname") == 0 && i + 1 < argc) {
            hostname = Jim_String(argv[++i]);
        }
        else if (strcmp(opt, "-instance") == 0 && i + 1 < argc) {
            instance = Jim_String(argv[++i]);
        }
        else {
            Jim_SetResultFormatted(interp, "unknown option \"%s\"", opt);
            return JIM_ERR;
        }
    }

    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "mdns_init failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    if (hostname) {
        err = mdns_hostname_set(hostname);
        if (err != ESP_OK) {
            mdns_free();
            Jim_SetResultFormatted(interp, "mdns_hostname_set failed: %s", esp_err_to_name(err));
            return JIM_ERR;
        }
        strncpy(mdns_state.hostname, hostname, sizeof(mdns_state.hostname) - 1);
    }

    if (instance) {
        err = mdns_instance_name_set(instance);
        if (err != ESP_OK) {
            mdns_free();
            Jim_SetResultFormatted(interp, "mdns_instance_name_set failed: %s", esp_err_to_name(err));
            return JIM_ERR;
        }
        strncpy(mdns_state.instance, instance, sizeof(mdns_state.instance) - 1);
    }

    mdns_state.initialized = 1;
    ESP_LOGI(TAG, "mDNS initialized");
    return JIM_OK;
}

static int mdns_cmd_deinit(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (!mdns_state.initialized) {
        Jim_SetResultString(interp, "mdns not initialized", -1);
        return JIM_ERR;
    }

    mdns_free();
    memset(&mdns_state, 0, sizeof(mdns_state));
    ESP_LOGI(TAG, "mDNS deinitialized");
    return JIM_OK;
}

static int mdns_cmd_service(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (!mdns_state.initialized) {
        Jim_SetResultString(interp, "mdns not initialized", -1);
        return JIM_ERR;
    }

    if (argc < 1) {
        Jim_SetResultString(interp, "usage: mdns service add|remove ...", -1);
        return JIM_ERR;
    }

    const char *action = Jim_String(argv[0]);

    if (strcmp(action, "add") == 0) {
        /* mdns service add <type> <proto> <port> ?-txt {k1 v1 k2 v2}? */
        if (argc < 4) {
            Jim_SetResultString(interp, "usage: mdns service add <type> <proto> <port> ?-txt {k v ...}?", -1);
            return JIM_ERR;
        }

        const char *type = Jim_String(argv[1]);
        const char *proto = Jim_String(argv[2]);
        long port;
        if (Jim_GetLong(interp, argv[3], &port) != JIM_OK) return JIM_ERR;

        /* Parse optional TXT records */
        mdns_txt_item_t *txt_items = NULL;
        int txt_count = 0;

        for (int i = 4; i < argc; i++) {
            const char *opt = Jim_String(argv[i]);
            if (strcmp(opt, "-txt") == 0 && i + 1 < argc) {
                Jim_Obj *txtList = argv[++i];
                int listLen = Jim_ListLength(interp, txtList);
                if (listLen % 2 != 0) {
                    Jim_SetResultString(interp, "-txt requires even number of elements {key val ...}", -1);
                    return JIM_ERR;
                }
                txt_count = listLen / 2;
                txt_items = calloc(txt_count, sizeof(mdns_txt_item_t));
                if (!txt_items) {
                    Jim_SetResultString(interp, "out of memory", -1);
                    return JIM_ERR;
                }
                for (int j = 0; j < txt_count; j++) {
                    txt_items[j].key = Jim_String(Jim_ListGetIndex(interp, txtList, j * 2));
                    txt_items[j].value = Jim_String(Jim_ListGetIndex(interp, txtList, j * 2 + 1));
                }
            }
        }

        esp_err_t err = mdns_service_add(NULL, type, proto, (uint16_t)port, txt_items, txt_count);
        free(txt_items);

        if (err != ESP_OK) {
            Jim_SetResultFormatted(interp, "mdns_service_add failed: %s", esp_err_to_name(err));
            return JIM_ERR;
        }

        ESP_LOGI(TAG, "Added service %s.%s on port %ld", type, proto, port);
        return JIM_OK;
    }
    else if (strcmp(action, "remove") == 0) {
        /* mdns service remove <type> <proto> */
        if (argc < 3) {
            Jim_SetResultString(interp, "usage: mdns service remove <type> <proto>", -1);
            return JIM_ERR;
        }

        const char *type = Jim_String(argv[1]);
        const char *proto = Jim_String(argv[2]);

        esp_err_t err = mdns_service_remove(type, proto);
        if (err != ESP_OK) {
            Jim_SetResultFormatted(interp, "mdns_service_remove failed: %s", esp_err_to_name(err));
            return JIM_ERR;
        }

        return JIM_OK;
    }
    else {
        Jim_SetResultFormatted(interp, "unknown service action \"%s\", expected add or remove", action);
        return JIM_ERR;
    }
}

static int mdns_cmd_query(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (!mdns_state.initialized) {
        Jim_SetResultString(interp, "mdns not initialized", -1);
        return JIM_ERR;
    }

    if (argc < 2) {
        Jim_SetResultString(interp, "usage: mdns query <service_type> <proto> ?-timeout ms?", -1);
        return JIM_ERR;
    }

    const char *service_type = Jim_String(argv[0]);
    const char *proto = Jim_String(argv[1]);
    long timeout_ms = 3000;

    for (int i = 2; i < argc; i++) {
        const char *opt = Jim_String(argv[i]);
        if (strcmp(opt, "-timeout") == 0 && i + 1 < argc) {
            if (Jim_GetLong(interp, argv[++i], &timeout_ms) != JIM_OK) return JIM_ERR;
        }
    }

    mdns_result_t *results = NULL;
    esp_err_t err = mdns_query_ptr(service_type, proto, (int)timeout_ms, 10, &results);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "mdns_query_ptr failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    Jim_Obj *resultList = Jim_NewListObj(interp, NULL, 0);

    mdns_result_t *r = results;
    while (r) {
        Jim_Obj *entry = Jim_NewListObj(interp, NULL, 0);

        if (r->hostname) {
            Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, "hostname", -1));
            Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, r->hostname, -1));
        }
        if (r->instance_name) {
            Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, "instance", -1));
            Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, r->instance_name, -1));
        }

        Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, "port", -1));
        Jim_ListAppendElement(interp, entry, Jim_NewIntObj(interp, r->port));

        /* IPv4 address if available */
        if (r->addr) {
            mdns_ip_addr_t *addr = r->addr;
            while (addr) {
                if (addr->addr.type == ESP_IPADDR_TYPE_V4) {
                    char ip_str[16];
                    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&addr->addr.u_addr.ip4));
                    Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, "ip", -1));
                    Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, ip_str, -1));
                    break;
                }
                addr = addr->next;
            }
        }

        /* TXT records */
        if (r->txt_count > 0 && r->txt) {
            Jim_Obj *txtList = Jim_NewListObj(interp, NULL, 0);
            for (int i = 0; i < (int)r->txt_count; i++) {
                Jim_ListAppendElement(interp, txtList,
                    Jim_NewStringObj(interp, r->txt[i].key ? r->txt[i].key : "", -1));
                Jim_ListAppendElement(interp, txtList,
                    Jim_NewStringObj(interp, r->txt[i].value ? r->txt[i].value : "", -1));
            }
            Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, "txt", -1));
            Jim_ListAppendElement(interp, entry, txtList);
        }

        Jim_ListAppendElement(interp, resultList, entry);
        r = r->next;
    }

    mdns_query_results_free(results);
    Jim_SetResult(interp, resultList);
    return JIM_OK;
}

static int mdns_cmd_resolve(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (!mdns_state.initialized) {
        Jim_SetResultString(interp, "mdns not initialized", -1);
        return JIM_ERR;
    }

    if (argc < 1) {
        Jim_SetResultString(interp, "usage: mdns resolve <hostname> ?-timeout ms?", -1);
        return JIM_ERR;
    }

    const char *hostname = Jim_String(argv[0]);
    long timeout_ms = 3000;

    for (int i = 1; i < argc; i++) {
        const char *opt = Jim_String(argv[i]);
        if (strcmp(opt, "-timeout") == 0 && i + 1 < argc) {
            if (Jim_GetLong(interp, argv[++i], &timeout_ms) != JIM_OK) return JIM_ERR;
        }
    }

    esp_ip4_addr_t addr;
    memset(&addr, 0, sizeof(addr));
    esp_err_t err = mdns_query_a(hostname, (int)timeout_ms, &addr);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NOT_FOUND) {
            Jim_SetResultString(interp, "", -1);
            return JIM_OK;
        }
        Jim_SetResultFormatted(interp, "mdns_query_a failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    char ip_str[16];
    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&addr));
    Jim_SetResultString(interp, ip_str, -1);
    return JIM_OK;
}

static int mdns_cmd_hostname(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (!mdns_state.initialized) {
        Jim_SetResultString(interp, "mdns not initialized", -1);
        return JIM_ERR;
    }

    if (argc == 0) {
        /* Get hostname */
        Jim_SetResultString(interp, mdns_state.hostname, -1);
        return JIM_OK;
    }

    /* Set hostname */
    const char *name = Jim_String(argv[0]);
    esp_err_t err = mdns_hostname_set(name);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "mdns_hostname_set failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }
    strncpy(mdns_state.hostname, name, sizeof(mdns_state.hostname) - 1);
    return JIM_OK;
}

static int mdns_cmd_status(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    Jim_Obj *result = Jim_NewListObj(interp, NULL, 0);

    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "initialized", -1));
    Jim_ListAppendElement(interp, result, Jim_NewIntObj(interp, mdns_state.initialized));

    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "hostname", -1));
    Jim_ListAppendElement(interp, result,
        Jim_NewStringObj(interp, mdns_state.hostname[0] ? mdns_state.hostname : "", -1));

    Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "instance", -1));
    Jim_ListAppendElement(interp, result,
        Jim_NewStringObj(interp, mdns_state.instance[0] ? mdns_state.instance : "", -1));

    Jim_SetResult(interp, result);
    return JIM_OK;
}

static const jim_subcmd_type mdns_command_table[] = {
    {   "init",
        "?-hostname name? ?-instance name?",
        mdns_cmd_init,
        0,
        -1,
        /* Description: Initialize mDNS subsystem */
    },
    {   "deinit",
        NULL,
        mdns_cmd_deinit,
        0,
        0,
        /* Description: Deinitialize mDNS */
    },
    {   "service",
        "add|remove ...",
        mdns_cmd_service,
        1,
        -1,
        /* Description: Add or remove mDNS services */
    },
    {   "query",
        "service_type proto ?-timeout ms?",
        mdns_cmd_query,
        2,
        -1,
        /* Description: Discover services on the network */
    },
    {   "resolve",
        "hostname ?-timeout ms?",
        mdns_cmd_resolve,
        1,
        -1,
        /* Description: Resolve a hostname to an IP address */
    },
    {   "hostname",
        "?name?",
        mdns_cmd_hostname,
        0,
        1,
        /* Description: Get or set mDNS hostname */
    },
    {   "status",
        NULL,
        mdns_cmd_status,
        0,
        0,
        /* Description: Return mDNS status */
    },
    { NULL }
};

int Jim_mdnsInit(Jim_Interp *interp)
{
    Jim_PackageProvideCheck(interp, "mdns");
    Jim_RegisterSubCmd(interp, "mdns", mdns_command_table, NULL);
    return JIM_OK;
}
