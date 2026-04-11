/* Jim Tcl MessagePack Extension for ESP32
 *
 * Provides MessagePack encode/decode using the embedded mpack library:
 *
 *   mpack encode <dict>    - convert a Jim Tcl dict to a byte list
 *   mpack decode <bytes>   - parse a byte list into a Jim Tcl dict
 *
 * Also exposes C-callable helpers for use by protocol extensions:
 *   jim_dict_to_mpack()  - dict -> mpack buffer (caller frees)
 *   jim_mpack_to_dict()  - mpack buffer -> dict (sets interp result)
 */

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "jim.h"
#include "jim-subcmd.h"
#include "jim-mpack.h"
#include "mpack/mpack.h"

/* ---------------------------------------------------------------------------
 * Internal: Write a Jim value into an mpack writer
 *
 * Type detection heuristic (same as json encoder):
 *   - "true"/"false" -> boolean
 *   - "null"         -> nil
 *   - Valid integer  -> int
 *   - Valid double   -> double
 *   - Even-length list with non-numeric first key -> map (recursive)
 *   - Odd-length list or numeric-first list -> array (recursive)
 *   - Everything else -> string
 * ---------------------------------------------------------------------------*/

static void jim_value_to_mpack(Jim_Interp *interp, Jim_Obj *obj, mpack_writer_t *writer);

static void jim_dict_to_mpack_writer(Jim_Interp *interp, Jim_Obj *dictObj, mpack_writer_t *writer)
{
    int len = Jim_ListLength(interp, dictObj);
    uint32_t count = (uint32_t)(len / 2);

    mpack_start_map(writer, count);

    for (int i = 0; i < len; i += 2) {
        Jim_Obj *keyObj = Jim_ListGetIndex(interp, dictObj, i);
        Jim_Obj *valObj = Jim_ListGetIndex(interp, dictObj, i + 1);
        const char *key = Jim_String(keyObj);

        mpack_write_cstr(writer, key);
        jim_value_to_mpack(interp, valObj, writer);
    }

    mpack_finish_map(writer);
}

static void jim_value_to_mpack(Jim_Interp *interp, Jim_Obj *obj, mpack_writer_t *writer)
{
    const char *str = Jim_String(obj);
    int slen;
    Jim_GetString(obj, &slen);

    /* Empty string */
    if (slen == 0) {
        mpack_write_cstr(writer, "");
        return;
    }

    /* Boolean */
    if (strcmp(str, "true") == 0) {
        mpack_write_bool(writer, true);
        return;
    }
    if (strcmp(str, "false") == 0) {
        mpack_write_bool(writer, false);
        return;
    }

    /* Null */
    if (strcmp(str, "null") == 0) {
        mpack_write_nil(writer);
        return;
    }

    /* Try integer */
    jim_wide wval;
    if (Jim_GetWide(interp, obj, &wval) == JIM_OK) {
        mpack_write_int(writer, (int64_t)wval);
        return;
    }

    /* Try double */
    double dval;
    if (Jim_GetDouble(interp, obj, &dval) == JIM_OK) {
        mpack_write_double(writer, dval);
        return;
    }

    /* Try as dict (even-length list with >= 2 elements, non-numeric first key) */
    int listlen = Jim_ListLength(interp, obj);
    if (listlen >= 2 && (listlen % 2) == 0) {
        Jim_Obj *firstKey = Jim_ListGetIndex(interp, obj, 0);
        jim_wide dummy;
        if (Jim_GetWide(interp, firstKey, &dummy) != JIM_OK) {
            jim_dict_to_mpack_writer(interp, obj, writer);
            return;
        }
    }

    /* Try as array (list with > 1 element) */
    if (listlen > 1) {
        mpack_start_array(writer, (uint32_t)listlen);
        for (int i = 0; i < listlen; i++) {
            Jim_Obj *elem = Jim_ListGetIndex(interp, obj, i);
            jim_value_to_mpack(interp, elem, writer);
        }
        mpack_finish_array(writer);
        return;
    }

    /* Default: string */
    mpack_write_cstr(writer, str);
}

/* ---------------------------------------------------------------------------
 * Internal: Convert an mpack node to a Jim value
 * ---------------------------------------------------------------------------*/

static Jim_Obj *mpack_node_to_jim(Jim_Interp *interp, mpack_node_t node)
{
    mpack_type_t type = mpack_node_type(node);

    switch (type) {
        case mpack_type_nil:
            return Jim_NewStringObj(interp, "", 0);

        case mpack_type_bool:
            return Jim_NewIntObj(interp, mpack_node_bool(node) ? 1 : 0);

        case mpack_type_int:
            return Jim_NewIntObj(interp, (jim_wide)mpack_node_i64(node));

        case mpack_type_uint:
            return Jim_NewIntObj(interp, (jim_wide)mpack_node_i64(node));

        case mpack_type_float:
            return Jim_NewDoubleObj(interp, (double)mpack_node_float(node));

        case mpack_type_double:
            return Jim_NewDoubleObj(interp, mpack_node_double(node));

        case mpack_type_str: {
            const char *s = mpack_node_str(node);
            size_t slen = mpack_node_strlen(node);
            return Jim_NewStringObj(interp, s, (int)slen);
        }

        case mpack_type_array: {
            size_t count = mpack_node_array_length(node);
            Jim_Obj *list = Jim_NewListObj(interp, NULL, 0);
            for (size_t i = 0; i < count; i++) {
                Jim_ListAppendElement(interp, list,
                    mpack_node_to_jim(interp, mpack_node_array_at(node, i)));
            }
            return list;
        }

        case mpack_type_map: {
            size_t count = mpack_node_map_count(node);
            Jim_Obj *dict = Jim_NewListObj(interp, NULL, 0);
            for (size_t i = 0; i < count; i++) {
                Jim_ListAppendElement(interp, dict,
                    mpack_node_to_jim(interp, mpack_node_map_key_at(node, i)));
                Jim_ListAppendElement(interp, dict,
                    mpack_node_to_jim(interp, mpack_node_map_value_at(node, i)));
            }
            return dict;
        }

        default:
            return Jim_NewStringObj(interp, "", 0);
    }
}

/* ---------------------------------------------------------------------------
 * C-callable helpers (declared in jim-mpack.h)
 * ---------------------------------------------------------------------------*/

uint8_t *jim_dict_to_mpack(Jim_Interp *interp, Jim_Obj *dictObj, size_t *outlen)
{
    int len = Jim_ListLength(interp, dictObj);
    if (len < 0 || (len % 2) != 0) {
        Jim_SetResultString(interp, "value is not a valid dict (odd element count)", -1);
        return NULL;
    }

    char *data = NULL;
    size_t size = 0;
    mpack_writer_t writer;
    mpack_writer_init_growable(&writer, &data, &size);

    jim_dict_to_mpack_writer(interp, dictObj, &writer);

    mpack_error_t err = mpack_writer_destroy(&writer);
    if (err != mpack_ok) {
        Jim_SetResultString(interp, "mpack encoding failed", -1);
        if (data) {
            MPACK_FREE(data);
        }
        return NULL;
    }

    *outlen = size;
    return (uint8_t *)data;
}

int jim_mpack_to_dict(Jim_Interp *interp, const uint8_t *data, size_t len)
{
    mpack_tree_t tree;
    mpack_tree_init_data(&tree, (const char *)data, len);
    mpack_tree_parse(&tree);

    if (mpack_tree_error(&tree) != mpack_ok) {
        mpack_tree_destroy(&tree);
        Jim_SetResultString(interp, "mpack parse error", -1);
        return JIM_ERR;
    }

    mpack_node_t root = mpack_tree_root(&tree);
    Jim_Obj *result = mpack_node_to_jim(interp, root);

    mpack_error_t err = mpack_tree_destroy(&tree);
    if (err != mpack_ok) {
        Jim_SetResultString(interp, "mpack tree error", -1);
        return JIM_ERR;
    }

    Jim_SetResult(interp, result);
    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Tcl command: mpack encode <dict>
 * ---------------------------------------------------------------------------*/

static int mpack_cmd_encode(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    size_t mpack_len = 0;
    uint8_t *buf = jim_dict_to_mpack(interp, argv[0], &mpack_len);
    if (!buf) {
        return JIM_ERR;
    }

    /* Convert binary buffer to space-separated decimal byte list */
    Jim_Obj *list = Jim_NewListObj(interp, NULL, 0);
    for (size_t i = 0; i < mpack_len; i++) {
        Jim_ListAppendElement(interp, list, Jim_NewIntObj(interp, buf[i]));
    }

    MPACK_FREE(buf);
    Jim_SetResult(interp, list);
    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Tcl command: mpack decode <bytes>
 * ---------------------------------------------------------------------------*/

static int mpack_cmd_decode(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    /* Parse the byte list into a binary buffer */
    int listlen = Jim_ListLength(interp, argv[0]);
    if (listlen < 0) {
        Jim_SetResultString(interp, "invalid byte list", -1);
        return JIM_ERR;
    }
    if (listlen == 0) {
        Jim_SetResultString(interp, "empty byte list", -1);
        return JIM_ERR;
    }

    uint8_t *buf = (uint8_t *)malloc(listlen);
    if (!buf) {
        Jim_SetResultString(interp, "out of memory", -1);
        return JIM_ERR;
    }

    for (int i = 0; i < listlen; i++) {
        Jim_Obj *elem = Jim_ListGetIndex(interp, argv[0], i);
        jim_wide val;
        if (Jim_GetWide(interp, elem, &val) != JIM_OK || val < 0 || val > 255) {
            free(buf);
            Jim_SetResultFormatted(interp, "invalid byte value at index %d", i);
            return JIM_ERR;
        }
        buf[i] = (uint8_t)val;
    }

    int rc = jim_mpack_to_dict(interp, buf, (size_t)listlen);
    free(buf);
    return rc;
}

/* ---------------------------------------------------------------------------
 * Format-string pack: mpack pack {format} ?args...?
 *
 * Format is a Tcl list of: key type key type ...
 * where type is one of:
 *   s = string, i = int32, I = uint32, q = int64, d = double,
 *   ? = bool, b = uint8, B = binary,
 *   m = map (arg is a Tcl dict, recursive heuristic encoding),
 *   a = array (arg is a Tcl list, recursive heuristic encoding)
 *
 * Each key-type pair consumes one Tcl arg.
 * If the format has just one element (a bare type with no key),
 * it encodes a top-level value (not wrapped in a map).
 * ---------------------------------------------------------------------------*/

/* Encode one arg into writer according to type char */
static int pack_one(Jim_Interp *interp, mpack_writer_t *writer,
                    char type, Jim_Obj *arg)
{
    switch (type) {
        case 's':
            mpack_write_cstr(writer, Jim_String(arg));
            break;
        case 'i': {
            long val;
            if (Jim_GetLong(interp, arg, &val) != JIM_OK) return JIM_ERR;
            mpack_write_i32(writer, (int32_t)val);
            break;
        }
        case 'I': {
            long val;
            if (Jim_GetLong(interp, arg, &val) != JIM_OK) return JIM_ERR;
            mpack_write_u32(writer, (uint32_t)val);
            break;
        }
        case 'q': {
            jim_wide val;
            if (Jim_GetWide(interp, arg, &val) != JIM_OK) return JIM_ERR;
            mpack_write_i64(writer, (int64_t)val);
            break;
        }
        case 'd': {
            double val;
            if (Jim_GetDouble(interp, arg, &val) != JIM_OK) return JIM_ERR;
            mpack_write_double(writer, val);
            break;
        }
        case '?': {
            long val;
            if (Jim_GetLong(interp, arg, &val) != JIM_OK) return JIM_ERR;
            mpack_write_bool(writer, val != 0);
            break;
        }
        case 'b': {
            long val;
            if (Jim_GetLong(interp, arg, &val) != JIM_OK) return JIM_ERR;
            mpack_write_u8(writer, (uint8_t)(val & 0xFF));
            break;
        }
        case 'B': {
            int len;
            const char *data = Jim_GetString(arg, &len);
            mpack_write_bin(writer, data, (uint32_t)len);
            break;
        }
        case 'm':
            /* Map: arg is a Tcl dict, encode recursively with heuristics */
            jim_dict_to_mpack_writer(interp, arg, writer);
            break;
        case 'a': {
            /* Array: arg is a Tcl list, encode each element with heuristics */
            int listlen = Jim_ListLength(interp, arg);
            mpack_start_array(writer, (uint32_t)listlen);
            for (int j = 0; j < listlen; j++) {
                Jim_Obj *elem = Jim_ListGetIndex(interp, arg, j);
                jim_value_to_mpack(interp, elem, writer);
            }
            mpack_finish_array(writer);
            break;
        }
        default:
            Jim_SetResultFormatted(interp, "unknown pack type '%c'", type);
            return JIM_ERR;
    }
    return JIM_OK;
}

static int mpack_cmd_pack(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc < 1) {
        Jim_SetResultString(interp, "wrong # args: should be \"mpack pack format ?args...?\"", -1);
        return JIM_ERR;
    }

    /* Parse format: list of {key type key type ...} or bare {type} */
    Jim_Obj *fmtObj = argv[0];
    int fmtlen = Jim_ListLength(interp, fmtObj);

    char *data = NULL;
    size_t size = 0;
    mpack_writer_t writer;
    mpack_writer_init_growable(&writer, &data, &size);

    int argidx = 1;  /* index into argv for values */

    if (fmtlen == 1) {
        /* Single bare type — encode one value, no map wrapper */
        const char *typestr = Jim_String(Jim_ListGetIndex(interp, fmtObj, 0));
        if (argidx >= argc) {
            mpack_writer_destroy(&writer);
            Jim_SetResultString(interp, "not enough arguments for format", -1);
            return JIM_ERR;
        }
        if (pack_one(interp, &writer, typestr[0], argv[argidx]) != JIM_OK) {
            mpack_writer_destroy(&writer);
            return JIM_ERR;
        }
    } else {
        /* Key-type pairs — build a map */
        if (fmtlen % 2 != 0) {
            mpack_writer_destroy(&writer);
            Jim_SetResultString(interp, "format must have even number of elements (key type pairs)", -1);
            return JIM_ERR;
        }

        int nfields = fmtlen / 2;
        mpack_start_map(&writer, (uint32_t)nfields);

        for (int i = 0; i < fmtlen; i += 2) {
            const char *key = Jim_String(Jim_ListGetIndex(interp, fmtObj, i));
            const char *typestr = Jim_String(Jim_ListGetIndex(interp, fmtObj, i + 1));
            char type = typestr[0];

            mpack_write_cstr(&writer, key);

            if (argidx >= argc) {
                mpack_writer_destroy(&writer);
                Jim_SetResultFormatted(interp, "not enough arguments: need value for key '%s'", key);
                return JIM_ERR;
            }

            if (pack_one(interp, &writer, type, argv[argidx]) != JIM_OK) {
                mpack_writer_destroy(&writer);
                return JIM_ERR;
            }
            argidx++;
        }

        mpack_finish_map(&writer);
    }

    mpack_error_t err = mpack_writer_destroy(&writer);
    if (err != mpack_ok) {
        Jim_SetResultString(interp, "mpack pack failed", -1);
        if (data) MPACK_FREE(data);
        return JIM_ERR;
    }

    /* Return as byte list */
    Jim_Obj *list = Jim_NewListObj(interp, NULL, 0);
    for (size_t i = 0; i < size; i++) {
        Jim_ListAppendElement(interp, list, Jim_NewIntObj(interp, (uint8_t)data[i]));
    }
    MPACK_FREE(data);
    Jim_SetResult(interp, list);
    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Format-string unpack: mpack unpack <bytes> {format}
 *
 * Format is a Tcl list of: key type key type ...
 * Returns a dict with the named fields.
 * If format is a bare {type}, returns the decoded value directly.
 * ---------------------------------------------------------------------------*/

/* Extract one value from mpack node according to type */
static Jim_Obj *unpack_one(Jim_Interp *interp, mpack_node_t node, char type)
{
    switch (type) {
        case 's': {
            const char *s = mpack_node_str(node);
            size_t slen = mpack_node_strlen(node);
            return Jim_NewStringObj(interp, s, (int)slen);
        }
        case 'i':
            return Jim_NewIntObj(interp, (jim_wide)mpack_node_i32(node));
        case 'I':
            return Jim_NewIntObj(interp, (jim_wide)mpack_node_u32(node));
        case 'q':
            return Jim_NewIntObj(interp, (jim_wide)mpack_node_i64(node));
        case 'd':
            return Jim_NewDoubleObj(interp, mpack_node_double(node));
        case '?':
            return Jim_NewIntObj(interp, mpack_node_bool(node) ? 1 : 0);
        case 'b':
            return Jim_NewIntObj(interp, mpack_node_u8(node));
        case 'B': {
            const char *bin = mpack_node_bin_data(node);
            size_t blen = mpack_node_bin_size(node);
            return Jim_NewStringObj(interp, bin, (int)blen);
        }
        case 'm':
        case 'a':
            /* Recursive decode using existing heuristic */
            return mpack_node_to_jim(interp, node);
        default:
            return Jim_NewStringObj(interp, "", 0);
    }
}

static int mpack_cmd_unpack(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc < 2) {
        Jim_SetResultString(interp, "wrong # args: should be \"mpack unpack bytes format\"", -1);
        return JIM_ERR;
    }

    /* Parse byte list to buffer */
    int listlen = Jim_ListLength(interp, argv[0]);
    if (listlen <= 0) {
        Jim_SetResultString(interp, "empty byte list", -1);
        return JIM_ERR;
    }

    uint8_t *buf = malloc(listlen);
    if (!buf) {
        Jim_SetResultString(interp, "out of memory", -1);
        return JIM_ERR;
    }

    for (int i = 0; i < listlen; i++) {
        Jim_Obj *elem = Jim_ListGetIndex(interp, argv[0], i);
        jim_wide val;
        if (Jim_GetWide(interp, elem, &val) != JIM_OK || val < 0 || val > 255) {
            free(buf);
            Jim_SetResultFormatted(interp, "invalid byte at index %d", i);
            return JIM_ERR;
        }
        buf[i] = (uint8_t)val;
    }

    /* Parse mpack */
    mpack_tree_t tree;
    mpack_tree_init_data(&tree, (const char *)buf, listlen);
    mpack_tree_parse(&tree);

    if (mpack_tree_error(&tree) != mpack_ok) {
        mpack_tree_destroy(&tree);
        free(buf);
        Jim_SetResultString(interp, "mpack parse error", -1);
        return JIM_ERR;
    }

    mpack_node_t root = mpack_tree_root(&tree);

    /* Parse format */
    Jim_Obj *fmtObj = argv[1];
    int fmtlen = Jim_ListLength(interp, fmtObj);

    if (fmtlen == 1) {
        /* Bare type — extract single value */
        const char *typestr = Jim_String(Jim_ListGetIndex(interp, fmtObj, 0));
        Jim_SetResult(interp, unpack_one(interp, root, typestr[0]));
    } else if (fmtlen % 2 == 0) {
        /* Key-type pairs — extract named fields from map */
        Jim_Obj *result = Jim_NewListObj(interp, NULL, 0);

        for (int i = 0; i < fmtlen; i += 2) {
            const char *key = Jim_String(Jim_ListGetIndex(interp, fmtObj, i));
            const char *typestr = Jim_String(Jim_ListGetIndex(interp, fmtObj, i + 1));

            mpack_node_t val_node = mpack_node_map_cstr_optional(root, key);
            Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, key, -1));

            if (mpack_node_is_missing(val_node)) {
                Jim_ListAppendElement(interp, result, Jim_NewStringObj(interp, "", 0));
            } else {
                Jim_ListAppendElement(interp, result,
                    unpack_one(interp, val_node, typestr[0]));
            }
        }

        Jim_SetResult(interp, result);
    } else {
        mpack_tree_destroy(&tree);
        free(buf);
        Jim_SetResultString(interp, "format must have even elements (key type pairs) or single type", -1);
        return JIM_ERR;
    }

    mpack_tree_destroy(&tree);
    free(buf);
    return JIM_OK;
}

/* ---------------------------------------------------------------------------
 * Subcommand table and init
 * ---------------------------------------------------------------------------*/

static const jim_subcmd_type mpack_command_table[] = {
    {   "encode",
        "dict",
        mpack_cmd_encode,
        1,
        1,
    },
    {   "decode",
        "bytes",
        mpack_cmd_decode,
        1,
        1,
    },
    {   "pack",
        "format ?args...?",
        mpack_cmd_pack,
        1,
        -1,
    },
    {   "unpack",
        "bytes format",
        mpack_cmd_unpack,
        2,
        2,
    },
    { NULL }
};

int Jim_mpackInit(Jim_Interp *interp)
{
    Jim_PackageProvideCheck(interp, "mpack");
    Jim_RegisterSubCmd(interp, "mpack", mpack_command_table, NULL);
    return JIM_OK;
}
