/*
 * lz.c - small LZ77 compression for the system files in /system (see lz.h).
 *
 * Greedy matching over hash chains of 4-byte sequences (the best of up to 8
 * candidates in the last 64 KiB): fast enough to pack the kernel image at
 * every ISO start, and a little better than LZ4.
 * The decoder checks every length and offset, so a damaged file can never
 * write outside its buffer.
 *
 * Builds on the host too (cc -DLZ_HOST) for tools/tests.
 */
#ifdef LZ_HOST
#include <stdlib.h>
#include <string.h>
#define LZ_ALLOC(n) malloc(n)
#define LZ_FREE(p)  free(p)
#else
#include "kernel.h"
#include "disk/diskfmt.h"
#define LZ_ALLOC(n) kmalloc(n)
#define LZ_FREE(p)  kfree(p)
#endif
#include "lz.h"

#define MIN_MATCH   4
#define HASH_BITS   16
#define MAX_OFFSET  65535
#define LAST_LITS   5          /* the stream ends with at least this many literals */
#define CHAIN_DEPTH 8          /* candidates tried per position                    */

uint32_t lz_bound(uint32_t n) { return n + n / 255 + 16; }

static inline uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline uint32_t hash4(uint32_t v) { return (v * 2654435761u) >> (32 - HASH_BITS); }

/* Write a length remainder (after the 15 in the token) as 255-bytes + rest. */
static uint8_t *put_len(uint8_t *o, uint32_t len) {
    while (len >= 255) { *o++ = 255; len -= 255; }
    *o++ = (uint8_t)len;
    return o;
}

static uint8_t *put_sequence(uint8_t *o, const uint8_t *lit, uint32_t nlit, uint32_t off, uint32_t mlen) {
    uint8_t *token = o++;
    uint32_t ml = mlen ? mlen - MIN_MATCH : 0;
    *token = (uint8_t)((nlit >= 15 ? 15 : nlit) << 4 | (mlen ? (ml >= 15 ? 15 : ml) : 0));
    if (nlit >= 15) o = put_len(o, nlit - 15);
    memcpy(o, lit, nlit);
    o += nlit;
    if (mlen) {
        *o++ = (uint8_t)off;
        *o++ = (uint8_t)(off >> 8);
        if (ml >= 15) o = put_len(o, ml - 15);
    }
    return o;
}

uint32_t lz_compress(const uint8_t *src, uint32_t n, uint8_t *dst, uint32_t cap) {
    if (cap < lz_bound(n)) return 0;
    uint8_t *o = dst;
    uint32_t anchor = 0;
    if (n > LAST_LITS + MIN_MATCH + 8) {
        /* head[h]: newest position with hash h; chain[p & 0xFFFF]: the one before it */
        uint32_t *head = LZ_ALLOC(sizeof(uint32_t) << HASH_BITS);
        uint32_t *chain = LZ_ALLOC(sizeof(uint32_t) * (MAX_OFFSET + 1));
        if (!head || !chain) { LZ_FREE(head); LZ_FREE(chain); return 0; }
        memset(head, 0xFF, sizeof(uint32_t) << HASH_BITS);
        uint32_t limit = n - LAST_LITS - MIN_MATCH;   /* last position a match may start at */
        uint32_t i = 0;
#define INSERT(p) do { uint32_t h_ = hash4(read32(src + (p))); chain[(p) & MAX_OFFSET] = head[h_]; head[h_] = (p); } while (0)
        while (i < limit) {
            /* longest match among the last CHAIN_DEPTH positions with the same hash
               (it must leave LAST_LITS bytes as literals) */
            uint32_t v = read32(src + i), best = 0, best_cand = 0, max = n - LAST_LITS - i;
            uint32_t cand = head[hash4(v)];
            for (int depth = 0; depth < CHAIN_DEPTH && cand != 0xFFFFFFFFu && i - cand <= MAX_OFFSET; depth++) {
                if (read32(src + cand) == v && src[cand + best] == src[i + best]) {
                    uint32_t len = MIN_MATCH;
                    while (len < max && src[cand + len] == src[i + len]) len++;
                    if (len > best) { best = len; best_cand = cand; if (len == max) break; }
                }
                uint32_t nx = chain[cand & MAX_OFFSET];
                if (nx >= cand) break;
                cand = nx;
            }
            INSERT(i);
            if (best < MIN_MATCH) { i++; continue; }
            cand = best_cand;
            uint32_t len = best;
            /* extend backwards into the pending literals */
            while (i > anchor && cand > 0 && src[i - 1] == src[cand - 1]) { i--; cand--; len++; }
            o = put_sequence(o, src + anchor, i - anchor, i - cand, len);
            /* index the positions inside the match, then continue after it */
            uint32_t end = i + len;
            for (uint32_t p = i + 1; p < end && p < limit; p++)
                INSERT(p);
            i = anchor = end;
        }
#undef INSERT
        LZ_FREE(head);
        LZ_FREE(chain);
    }
    o = put_sequence(o, src + anchor, n - anchor, 0, 0);
    return (uint32_t)(o - dst);
}

int32_t lz_decompress(const uint8_t *src, uint32_t n, uint8_t *dst, uint32_t cap) {
    const uint8_t *ip = src, *iend = src + n;
    uint8_t *op = dst, *oend = dst + cap;
    while (ip < iend) {
        uint32_t token = *ip++;
        uint32_t nlit = token >> 4;
        if (nlit == 15) {
            uint32_t b;
            do {
                if (ip >= iend) return -1;
                b = *ip++;
                nlit += b;
                if (nlit > cap) return -1;
            } while (b == 255);
        }
        if (nlit > (uint32_t)(iend - ip) || nlit > (uint32_t)(oend - op)) return -1;
        memcpy(op, ip, nlit);
        op += nlit;
        ip += nlit;
        if (ip >= iend) break;                  /* the last sequence has literals only */
        if (iend - ip < 2) return -1;
        uint32_t off = (uint32_t)ip[0] | (uint32_t)ip[1] << 8;
        ip += 2;
        if (off == 0 || off > (uint32_t)(op - dst)) return -1;
        uint32_t mlen = (token & 15);
        if (mlen == 15) {
            uint32_t b;
            do {
                if (ip >= iend) return -1;
                b = *ip++;
                mlen += b;
                if (mlen > cap) return -1;
            } while (b == 255);
        }
        mlen += MIN_MATCH;
        if (mlen > (uint32_t)(oend - op)) return -1;
        const uint8_t *m = op - off;
        if (off >= mlen) { memcpy(op, m, mlen); op += mlen; }
        else while (mlen--) *op++ = *m++;      /* overlapping copy: repeats the pattern */
    }
    return (int32_t)(op - dst);
}

#ifndef LZ_HOST
/* ---- NVLZ container ------------------------------------------------------------------ */
bool nvlz_is(const void *data, uint32_t len) {
    return data && len >= NVLZ_HEADER && !memcmp(data, "NVLZ", 4);
}

uint8_t *nvlz_pack(const void *data, uint32_t len, uint32_t *out_len) {
    uint32_t cap = NVLZ_HEADER + lz_bound(len);
    uint8_t *out = kmalloc(cap);
    if (!out) return NULL;
    uint32_t z = lz_compress(data, len, out + NVLZ_HEADER, cap - NVLZ_HEADER);
    if (!z) { kfree(out); return NULL; }
    uint32_t crc = crc32_buf(data, len);
    memcpy(out, "NVLZ", 4);
    memcpy(out + 4, &len, 4);
    memcpy(out + 8, &crc, 4);
    *out_len = NVLZ_HEADER + z;
    /* give the unused tail back */
    uint8_t *shrunk = krealloc(out, *out_len);
    return shrunk ? shrunk : out;
}

uint8_t *nvlz_unpack(const void *data, uint32_t len, uint32_t *out_len) {
    if (!nvlz_is(data, len)) return NULL;
    const uint8_t *p = data;
    uint32_t raw, crc;
    memcpy(&raw, p + 4, 4);
    memcpy(&crc, p + 8, 4);
    if (raw > 64u * 1024 * 1024) return NULL;
    uint8_t *out = kmalloc(raw ? raw : 1);
    if (!out) return NULL;
    int32_t got = lz_decompress(p + NVLZ_HEADER, len - NVLZ_HEADER, out, raw);
    if (got != (int32_t)raw || crc32_buf(out, raw) != crc) { kfree(out); return NULL; }
    *out_len = raw;
    return out;
}
#endif
