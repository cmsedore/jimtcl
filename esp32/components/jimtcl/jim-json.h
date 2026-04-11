/* Jim Tcl JSON Extension Header
 *
 * C-callable helpers for converting between Jim Tcl dicts and JSON strings.
 * Used by protocol extensions (HTTP, MQTT, WebSocket) when -json flag is present.
 */

#ifndef JIM_JSON_H
#define JIM_JSON_H

#include "jim.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Convert a Jim dict object to a JSON string.
 * Caller must free() the result.
 * Returns NULL on error (sets interp result with error message).
 */
char *jim_dict_to_json(Jim_Interp *interp, Jim_Obj *dictObj);

/**
 * Parse a JSON string and set the interp result to a Jim dict.
 * @param json  The JSON string to parse
 * @param len   Length of JSON string, or -1 for strlen
 * Returns JIM_OK on success, JIM_ERR on parse failure.
 */
int jim_json_to_dict(Jim_Interp *interp, const char *json, int len);

#ifdef __cplusplus
}
#endif

#endif /* JIM_JSON_H */
