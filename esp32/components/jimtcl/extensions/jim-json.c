/* Jim Tcl JSON Extension for ESP32
 *
 * Provides JSON encode/decode using ESP-IDF's bundled cJSON library:
 *
 *   json encode <dict>         - convert a Jim Tcl dict to a JSON string
 *   json decode <json_string>  - parse a JSON string into a Jim Tcl dict
 *
 * Also exposes C-callable helpers for use by protocol extensions:
 *   jim_dict_to_json()   - dict -> JSON string (caller frees)
 *   jim_json_to_dict()   - JSON string -> dict (sets interp result)
 */

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "jim.h"
#include "jim-subcmd.h"
#include "jim-json.h"

#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "jim-json";

/* ---------------------------------------------------------------------------
 * Internal: Convert a Jim value to a cJSON node
 *
 * Type detection heuristic:
 *   - "true"/"false" -> boolean
 *   - Valid integer   -> number (int)
 *   - Valid double    -> number (double)
 *   - Even-length list that looks like key-value pairs -> object (recursive)
 *   - Odd-length list or non-dict list -> array (recursive)
 *   - Everything else -> string
 * ---------------------------------------------------------------------------*/

static cJSON *jim_value_to_cjson(Jim_Interp *interp, Jim_Obj *obj);

static cJSON *jim_dict_to_cjson(Jim_Interp *interp, Jim_Obj *dictObj)
{
    int len = Jim_ListLength(interp, dictObj);
    if (len < 0 || (len % 2) != 0) {
        return NULL;
    }

    cJSON *jobj = cJSON_CreateObject();
    if (!jobj) return NULL;

    for (int i = 0; i < len; i += 2) {
        Jim_Obj *keyObj = Jim_ListGetIndex(interp, dictObj, i);
        Jim_Obj *valObj = Jim_ListGetIndex(interp, dictObj, i + 1);
        const char *key = Jim_String(keyObj);

        cJSON *jval = jim_value_to_cjson(interp, valObj);
        if (!jval) {
            jval = cJSON_CreateString(Jim_String(valObj));
        }
        if (jval) {
            cJSON_AddItemToObject(jobj, key, jval);
        }
    }

    return jobj;
}

static cJSON *jim_value_to_cjson(Jim_Interp *interp, Jim_Obj *obj)
{
    const char *str = Jim_String(obj);
    int slen;
    Jim_GetString(obj, &slen);

    /* Empty string */
    if (slen == 0) {
        return cJSON_CreateString("");
    }

    /* Boolean */
    if (strcmp(str, "true") == 0) {
        return cJSON_CreateTrue();
    }
    if (strcmp(str, "false") == 0) {
        return cJSON_CreateFalse();
    }

    /* Null */
    if (strcmp(str, "null") == 0) {
        return cJSON_CreateNull();
    }

    /* Try integer */
    jim_wide wval;
    if (Jim_GetWide(interp, obj, &wval) == JIM_OK) {
        return cJSON_CreateNumber((double)wval);
    }

    /* Try double */
    double dval;
    if (Jim_GetDouble(interp, obj, &dval) == JIM_OK) {
        return cJSON_CreateNumber(dval);
    }

    /* Try as dict (even-length list with >= 2 elements) */
    int listlen = Jim_ListLength(interp, obj);
    if (listlen >= 2 && (listlen % 2) == 0) {
        /* Check if first element looks like a string key (not purely numeric) */
        Jim_Obj *firstKey = Jim_ListGetIndex(interp, obj, 0);
        jim_wide dummy;
        if (Jim_GetWide(interp, firstKey, &dummy) != JIM_OK) {
            /* First key is not a number, treat as dict */
            cJSON *jobj = jim_dict_to_cjson(interp, obj);
            if (jobj) return jobj;
        }
    }

    /* Try as array (list with > 1 element) */
    if (listlen > 1) {
        cJSON *jarr = cJSON_CreateArray();
        if (jarr) {
            for (int i = 0; i < listlen; i++) {
                Jim_Obj *elem = Jim_ListGetIndex(interp, obj, i);
                cJSON *jelem = jim_value_to_cjson(interp, elem);
                if (!jelem) {
                    jelem = cJSON_CreateString(Jim_String(elem));
                }
                if (jelem) {
                    cJSON_AddItemToArray(jarr, jelem);
                }
            }
            return jarr;
        }
    }

    /* Default: string */
    return cJSON_CreateString(str);
}

/* ---------------------------------------------------------------------------
 * Internal: Convert a cJSON tree to a Jim value
 * ---------------------------------------------------------------------------*/

static Jim_Obj *cjson_to_jim(Jim_Interp *interp, cJSON *item)
{
    if (!item) {
        return Jim_NewStringObj(interp, "", 0);
    }

    switch (item->type & 0xFF) {
        case cJSON_False:
            return Jim_NewIntObj(interp, 0);

        case cJSON_True:
            return Jim_NewIntObj(interp, 1);

        case cJSON_NULL:
            return Jim_NewStringObj(interp, "", 0);

        case cJSON_Number: {
            /* Use integer if the value is integral */
            if (item->valuedouble == (double)(jim_wide)item->valuedouble &&
                fabs(item->valuedouble) < 9e18) {
                return Jim_NewIntObj(interp, (jim_wide)item->valuedouble);
            }
            return Jim_NewDoubleObj(interp, item->valuedouble);
        }

        case cJSON_String:
            return Jim_NewStringObj(interp, item->valuestring, -1);

        case cJSON_Array: {
            Jim_Obj *list = Jim_NewListObj(interp, NULL, 0);
            cJSON *child;
            cJSON_ArrayForEach(child, item) {
                Jim_ListAppendElement(interp, list, cjson_to_jim(interp, child));
            }
            return list;
        }

        case cJSON_Object: {
            Jim_Obj *dict = Jim_NewListObj(interp, NULL, 0);
            cJSON *child;
            cJSON_ArrayForEach(child, item) {
                Jim_ListAppendElement(interp, dict,
                    Jim_NewStringObj(interp, child->string, -1));
                Jim_ListAppendElement(interp, dict,
                    cjson_to_jim(interp, child));
            }
            return dict;
        }

        default:
            return Jim_NewStringObj(interp, "", 0);
    }
}

/* ---------------------------------------------------------------------------
 * C-callable helpers (declared in jim-json.h)
 * ---------------------------------------------------------------------------*/

char *jim_dict_to_json(Jim_Interp *interp, Jim_Obj *dictObj)
{
    int len = Jim_ListLength(interp, dictObj);
    if (len < 0 || (len % 2) != 0) {
        Jim_SetResultString(interp, "value is not a valid dict (odd element count)", -1);
        return NULL;
    }

    cJSON *jobj = jim_dict_to_cjson(interp, dictObj);
    if (!jobj) {
        Jim_SetResultString(interp, "failed to build JSON object from dict", -1);
        return NULL;
    }

    char *json_str = cJSON_PrintUnformatted(jobj);
    cJSON_Delete(jobj);

    if (!json_str) {
        Jim_SetResultString(interp, "failed to serialize JSON", -1);
        return NULL;
    }

    return json_str;
}

int jim_json_to_dict(Jim_Interp *interp, const char *json, int len)
{
    /* cJSON needs a NUL-terminated string. If len >= 0 we may need to copy. */
    cJSON *root;
    if (len >= 0) {
        char *tmp = malloc(len + 1);
        if (!tmp) {
            Jim_SetResultString(interp, "out of memory parsing JSON", -1);
            return JIM_ERR;
        }
        memcpy(tmp, json, len);
        tmp[len] = '\0';
        root = cJSON_Parse(tmp);
        free(tmp);
    } else {
        root = cJSON_Parse(json);
    }

    if (!root) {
        const char *err = cJSON_GetErrorPtr();
        if (err) {
            Jim_SetResultFormatted(interp, "JSON parse error near: %.40s", err);
        } else {
            Jim_SetResultString(interp, "JSON parse error", -1);
        }
        return JIM_ERR;
    }

    Jim_Obj *result = cjson_to_jim(interp, root);
    cJSON_Delete(root);

    Jim_SetResult(interp, result);
    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Tcl command: json encode <dict>
 * ---------------------------------------------------------------------------*/

static int json_cmd_encode(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    char *json_str = jim_dict_to_json(interp, argv[0]);
    if (!json_str) {
        return JIM_ERR;
    }

    Jim_SetResultString(interp, json_str, -1);
    free(json_str);
    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Tcl command: json decode <json_string>
 * ---------------------------------------------------------------------------*/

static int json_cmd_decode(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    int len;
    const char *json_str = Jim_GetString(argv[0], &len);

    return jim_json_to_dict(interp, json_str, len);
}

/* ---------------------------------------------------------------------------
 * Subcommand table and init
 * ---------------------------------------------------------------------------*/

static const jim_subcmd_type json_command_table[] = {
    {   "encode",
        "dict",
        json_cmd_encode,
        1,
        1,
    },
    {   "decode",
        "json_string",
        json_cmd_decode,
        1,
        1,
    },
    { NULL }
};

int Jim_jsonInit(Jim_Interp *interp)
{
    Jim_PackageProvideCheck(interp, "json");
    Jim_RegisterSubCmd(interp, "json", json_command_table, NULL);
    return JIM_OK;
}
