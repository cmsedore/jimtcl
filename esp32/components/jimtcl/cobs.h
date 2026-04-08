#ifndef COBS_H
#define COBS_H
#include <stddef.h>
#include <stdint.h>

// Encode src into dst using COBS. Returns encoded length (NOT including delimiter).
// dst must be at least srclen + srclen/254 + 1 bytes.
size_t cobs_encode(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen);

// Decode a COBS frame (without the 0x00 delimiter) from src into dst.
// Returns decoded length, or 0 on error.
size_t cobs_decode(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen);

// Maximum encoded size for a given input size
#define COBS_MAX_ENCODED_SIZE(n) ((n) + ((n) / 254) + 1)

#endif
