/*
 * lz.h - small LZ77 compression for the system files in /system (internal).
 *
 * The stream format is LZ4's block format: sequences of
 *   token (high nibble: literal count, low nibble: match length - 4; 15 = more
 *   length bytes follow, each adding up to 255), the literals, a 16-bit little
 *   endian match offset (1..65535) and the extra match length bytes; the last
 *   sequence has literals only.
 *
 * Files keep it in an "NVLZ" container so they can be told apart from plain
 * files and checked:
 *   "NVLZ" | u32 raw length | u32 CRC32 of the raw data | stream
 */
#ifndef FS_LZ_H
#define FS_LZ_H

#include <stdint.h>
#include <stdbool.h>

#define NVLZ_HEADER 12

/* Largest possible stream for n input bytes. */
uint32_t lz_bound(uint32_t n);
/* Compress n bytes into dst (cap >= lz_bound(n)). Returns the stream length, 0 on error. */
uint32_t lz_compress(const uint8_t *src, uint32_t n, uint8_t *dst, uint32_t cap);
/* Decompress a stream. Returns the number of bytes written, or -1 if the stream is bad
   or does not fit into cap. */
int32_t  lz_decompress(const uint8_t *src, uint32_t n, uint8_t *dst, uint32_t cap);

#ifndef LZ_HOST
/* NVLZ container (kmalloc'd results, NULL on error / bad data). */
bool     nvlz_is(const void *data, uint32_t len);
uint8_t *nvlz_pack(const void *data, uint32_t len, uint32_t *out_len);
uint8_t *nvlz_unpack(const void *data, uint32_t len, uint32_t *out_len);
#endif

#endif
