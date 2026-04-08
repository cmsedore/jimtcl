#ifndef JIM_MPACK_H
#define JIM_MPACK_H
#include "jim.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Convert a Jim dict to an mpack buffer. Caller must free().
 * Returns NULL on error (sets interp result).
 */
uint8_t *jim_dict_to_mpack(Jim_Interp *interp, Jim_Obj *dictObj, size_t *outlen);

/**
 * Parse mpack buffer and set interp result to a Jim dict/value.
 * Returns JIM_OK or JIM_ERR.
 */
int jim_mpack_to_dict(Jim_Interp *interp, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* JIM_MPACK_H */
