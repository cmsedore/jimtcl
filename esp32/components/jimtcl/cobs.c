/* COBS (Consistent Overhead Byte Stuffing) framing
 *
 * Encodes/decodes binary data so that 0x00 never appears in the output,
 * allowing 0x00 to be used as an unambiguous frame delimiter.
 *
 * Reference: https://en.wikipedia.org/wiki/Consistent_Overhead_Byte_Stuffing
 */

#include "cobs.h"
#include <string.h>

size_t cobs_encode(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen)
{
    size_t max_needed = COBS_MAX_ENCODED_SIZE(srclen);
    if (dstlen < max_needed) {
        return 0;
    }

    size_t read_idx = 0;
    size_t write_idx = 1;  /* Reserve first byte for overhead code */
    size_t code_idx = 0;   /* Position of the current overhead byte */
    uint8_t code = 1;      /* Distance to next zero (or end of block) */

    while (read_idx < srclen) {
        if (src[read_idx] == 0x00) {
            /* Found a zero: write the current code and start a new block */
            dst[code_idx] = code;
            code_idx = write_idx++;
            code = 1;
            read_idx++;
        } else {
            /* Copy non-zero byte */
            dst[write_idx++] = src[read_idx++];
            code++;
            if (code == 0xFF) {
                /* Block of 254 non-zero bytes: write code and start new block */
                dst[code_idx] = code;
                code_idx = write_idx++;
                code = 1;
            }
        }
    }

    /* Write the final code byte */
    dst[code_idx] = code;

    return write_idx;
}

size_t cobs_decode(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen)
{
    if (srclen == 0) {
        return 0;
    }

    size_t read_idx = 0;
    size_t write_idx = 0;

    while (read_idx < srclen) {
        uint8_t code = src[read_idx++];

        if (code == 0) {
            /* Zero byte in encoded data is invalid */
            return 0;
        }

        /* Copy (code - 1) data bytes */
        for (uint8_t i = 1; i < code; i++) {
            if (read_idx >= srclen) {
                /* Unexpected end of input */
                return 0;
            }
            if (write_idx >= dstlen) {
                return 0;
            }
            if (src[read_idx] == 0x00) {
                /* Zero in data portion is invalid */
                return 0;
            }
            dst[write_idx++] = src[read_idx++];
        }

        /* If code != 0xFF and we haven't consumed all input, emit a zero */
        if (code != 0xFF && read_idx < srclen) {
            if (write_idx >= dstlen) {
                return 0;
            }
            dst[write_idx++] = 0x00;
        }
    }

    return write_idx;
}
