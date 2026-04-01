/* Jim Tcl NVS Extension for ESP32
 *
 * Provides Tcl commands for non-volatile storage:
 *
 *   nvs open <namespace> ?readonly?
 *   nvs close <handle>
 *   nvs set <handle> <key> <value> ?-type str|int|blob?
 *   nvs get <handle> <key> ?-type str|int|blob?
 *   nvs delete <handle> <key>
 *   nvs list <handle>
 *   nvs erase <handle>
 *   nvs commit <handle>
 */

#include <string.h>
#include <stdlib.h>
#include "jim.h"
#include "jim-subcmd.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "jim-nvs";

/* Simple handle tracking */
#define NVS_MAX_HANDLES 8

typedef struct {
    nvs_handle_t handle;
    int in_use;
    char namespace[16];
} nvs_slot_t;

static nvs_slot_t nvs_slots[NVS_MAX_HANDLES] = { 0 };
static int nvs_initialized = 0;

static void ensure_nvs_init(void)
{
    if (!nvs_initialized) {
        esp_err_t err = nvs_flash_init();
        if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            nvs_flash_erase();
            err = nvs_flash_init();
        }
        if (err == ESP_OK) {
            nvs_initialized = 1;
        }
    }
}

static int find_free_slot(void)
{
    for (int i = 0; i < NVS_MAX_HANDLES; i++) {
        if (!nvs_slots[i].in_use) return i;
    }
    return -1;
}

static int get_slot(Jim_Interp *interp, Jim_Obj *obj, nvs_slot_t **slot)
{
    long idx;
    if (Jim_GetLong(interp, obj, &idx) != JIM_OK) {
        return JIM_ERR;
    }
    if (idx < 0 || idx >= NVS_MAX_HANDLES || !nvs_slots[idx].in_use) {
        Jim_SetResultFormatted(interp, "invalid NVS handle: %ld", idx);
        return JIM_ERR;
    }
    *slot = &nvs_slots[idx];
    return JIM_OK;
}

static int nvs_cmd_open(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    ensure_nvs_init();

    const char *ns = Jim_String(argv[0]);
    int readonly = 0;
    if (argc >= 2) {
        const char *mode = Jim_String(argv[1]);
        if (strcmp(mode, "readonly") == 0) {
            readonly = 1;
        }
    }

    int slot_idx = find_free_slot();
    if (slot_idx < 0) {
        Jim_SetResultString(interp, "no free NVS handle slots", -1);
        return JIM_ERR;
    }

    nvs_slot_t *slot = &nvs_slots[slot_idx];
    esp_err_t err = nvs_open(ns, readonly ? NVS_READONLY : NVS_READWRITE, &slot->handle);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "nvs_open failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    slot->in_use = 1;
    strncpy(slot->namespace, ns, sizeof(slot->namespace) - 1);
    slot->namespace[sizeof(slot->namespace) - 1] = '\0';

    Jim_SetResultInt(interp, slot_idx);
    return JIM_OK;
}

static int nvs_cmd_close(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    nvs_slot_t *slot;
    if (get_slot(interp, argv[0], &slot) != JIM_OK) return JIM_ERR;

    nvs_close(slot->handle);
    slot->in_use = 0;
    return JIM_OK;
}

static int nvs_cmd_set(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    nvs_slot_t *slot;
    if (get_slot(interp, argv[0], &slot) != JIM_OK) return JIM_ERR;

    const char *key = Jim_String(argv[1]);
    const char *type = "str";

    if (argc >= 4) {
        const char *opt = Jim_String(argv[3]);
        if (strcmp(opt, "-type") == 0 && argc >= 5) {
            type = Jim_String(argv[4]);
        } else {
            type = opt; /* Allow: nvs set h key val str */
        }
    }

    esp_err_t err;
    if (strcmp(type, "int") == 0 || strcmp(type, "i32") == 0) {
        long val;
        if (Jim_GetLong(interp, argv[2], &val) != JIM_OK) return JIM_ERR;
        err = nvs_set_i32(slot->handle, key, (int32_t)val);
    } else {
        /* Default: string */
        const char *val = Jim_String(argv[2]);
        err = nvs_set_str(slot->handle, key, val);
    }

    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "nvs set failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    /* Auto-commit */
    nvs_commit(slot->handle);
    return JIM_OK;
}

static int nvs_cmd_get(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    nvs_slot_t *slot;
    if (get_slot(interp, argv[0], &slot) != JIM_OK) return JIM_ERR;

    const char *key = Jim_String(argv[1]);
    const char *type = "str";

    if (argc >= 3) {
        type = Jim_String(argv[2]);
    }

    if (strcmp(type, "int") == 0 || strcmp(type, "i32") == 0) {
        int32_t val;
        esp_err_t err = nvs_get_i32(slot->handle, key, &val);
        if (err != ESP_OK) {
            Jim_SetResultFormatted(interp, "nvs get failed: %s", esp_err_to_name(err));
            return JIM_ERR;
        }
        Jim_SetResultInt(interp, val);
    } else {
        /* String */
        size_t required_size = 0;
        esp_err_t err = nvs_get_str(slot->handle, key, NULL, &required_size);
        if (err != ESP_OK) {
            Jim_SetResultFormatted(interp, "nvs get failed: %s", esp_err_to_name(err));
            return JIM_ERR;
        }
        char *buf = malloc(required_size);
        if (!buf) {
            Jim_SetResultString(interp, "out of memory", -1);
            return JIM_ERR;
        }
        err = nvs_get_str(slot->handle, key, buf, &required_size);
        if (err != ESP_OK) {
            free(buf);
            Jim_SetResultFormatted(interp, "nvs get failed: %s", esp_err_to_name(err));
            return JIM_ERR;
        }
        Jim_SetResultString(interp, buf, -1);
        free(buf);
    }

    return JIM_OK;
}

static int nvs_cmd_delete(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    nvs_slot_t *slot;
    if (get_slot(interp, argv[0], &slot) != JIM_OK) return JIM_ERR;

    const char *key = Jim_String(argv[1]);
    esp_err_t err = nvs_erase_key(slot->handle, key);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "nvs delete failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }
    nvs_commit(slot->handle);
    return JIM_OK;
}

static int nvs_cmd_erase(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    nvs_slot_t *slot;
    if (get_slot(interp, argv[0], &slot) != JIM_OK) return JIM_ERR;

    esp_err_t err = nvs_erase_all(slot->handle);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "nvs erase failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }
    nvs_commit(slot->handle);
    return JIM_OK;
}

static int nvs_cmd_commit(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    nvs_slot_t *slot;
    if (get_slot(interp, argv[0], &slot) != JIM_OK) return JIM_ERR;

    esp_err_t err = nvs_commit(slot->handle);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "nvs commit failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }
    return JIM_OK;
}

static const jim_subcmd_type nvs_command_table[] = {
    {   "open",
        "namespace ?readonly?",
        nvs_cmd_open,
        1,
        2,
        /* Description: Open NVS namespace, returns handle */
    },
    {   "close",
        "handle",
        nvs_cmd_close,
        1,
        1,
        /* Description: Close NVS handle */
    },
    {   "set",
        "handle key value ?type?",
        nvs_cmd_set,
        3,
        5,
        /* Description: Set a key-value pair (type: str or int) */
    },
    {   "get",
        "handle key ?type?",
        nvs_cmd_get,
        2,
        3,
        /* Description: Get a value by key (type: str or int) */
    },
    {   "delete",
        "handle key",
        nvs_cmd_delete,
        2,
        2,
        /* Description: Delete a key */
    },
    {   "erase",
        "handle",
        nvs_cmd_erase,
        1,
        1,
        /* Description: Erase all keys in namespace */
    },
    {   "commit",
        "handle",
        nvs_cmd_commit,
        1,
        1,
        /* Description: Explicitly commit pending changes */
    },
    { NULL }
};

int Jim_nvsInit(Jim_Interp *interp)
{
    Jim_PackageProvideCheck(interp, "nvs");
    Jim_RegisterSubCmd(interp, "nvs", nvs_command_table, NULL);
    return JIM_OK;
}
