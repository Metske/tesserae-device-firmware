/* frame_codec.c -- Bounds-checked LZ4 block codec + TSZ1 frame envelope. */

#include "frame_codec.h"

#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "app_config.h"
#include "esp_heap_caps.h"
#endif

#define TSZ_VERSION   1u
#define TSZ_CODEC_RAW 0u
#define TSZ_CODEC_LZ4 1u

#define LZ4_HASH_LOG   12
#define LZ4_HASH_SIZE  (1u << LZ4_HASH_LOG)
#define LZ4_MIN_MATCH  4u
#define LZ4_MAX_OFFSET 65535u
#define LZ4_LAST_LITS  5u
#define LZ4_MF_LIMIT   12u

static void *codec_alloc(size_t n)
{
#ifdef ESP_PLATFORM
    void *p = heap_caps_malloc(n, TESSERAE_FB_CAPS);
    if (p) return p;
#endif
    return malloc(n);
}

static uint32_t read_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t lz4_hash4(uint32_t v)
{
    return (v * 2654435761u) >> (32 - LZ4_HASH_LOG);
}

/* Compress `src[0..src_len)` into `dst[0..dst_cap)`. Returns compressed size,
 * or 0 if `dst_cap` is exceeded (incompressible -> caller uses RAW fallback). */
static size_t lz4_compress_bounded(const uint8_t *src, size_t src_len,
                                   uint8_t *dst, size_t dst_cap)
{
    if (src_len == 0 || dst_cap == 0) return 0;

    uint32_t *htab = calloc(LZ4_HASH_SIZE, sizeof(uint32_t));
    if (!htab) return 0;

    const uint8_t *ip = src;
    const uint8_t *anchor = src;
    const uint8_t *const iend = src + src_len;
    const uint8_t *const mflimit = (src_len > LZ4_MF_LIMIT) ? (iend - LZ4_MF_LIMIT) : src;
    const uint8_t *const matchlimit = (src_len > LZ4_LAST_LITS) ? (iend - LZ4_LAST_LITS) : src;

    uint8_t *op = dst;
    uint8_t *const oend = dst + dst_cap;

    if (src_len >= LZ4_MF_LIMIT) {
        ip++;
        while (ip < mflimit) {
            uint32_t seq = read_u32_le(ip);
            uint32_t h = lz4_hash4(seq);
            const uint8_t *ref = src + htab[h];
            htab[h] = (uint32_t)(ip - src);

            size_t dist = (size_t)(ip - ref);
            if (dist == 0 || dist > LZ4_MAX_OFFSET || read_u32_le(ref) != seq) {
                /* Step faster across incompressible runs */
                size_t skip = 1u + ((size_t)(ip - anchor) >> 5);
                ip += skip;
                continue;
            }

            /* Expand match forward */
            size_t lit_len = (size_t)(ip - anchor);
            const uint8_t *m_ip = ip + LZ4_MIN_MATCH;
            const uint8_t *m_ref = ref + LZ4_MIN_MATCH;
            while (m_ip < matchlimit && *m_ip == *m_ref) {
                m_ip++;
                m_ref++;
            }
            size_t match_len = (size_t)(m_ip - ip);

            /* Worst-case sequence bytes: 1 token + lit_len/255 + 1 + lit_len + 2 offset + match_len/255 + 1 */
            size_t need = 1u + (lit_len / 255u) + 1u + lit_len + 2u + (match_len / 255u) + 1u;
            if ((size_t)(oend - op) < need) {
                free(htab);
                return 0;
            }

            uint8_t *token = op++;
            uint8_t lit_tag = (lit_len >= 15u) ? 15u : (uint8_t)lit_len;
            size_t m_extra = match_len - LZ4_MIN_MATCH;
            uint8_t match_tag = (m_extra >= 15u) ? 15u : (uint8_t)m_extra;
            *token = (uint8_t)((lit_tag << 4) | match_tag);

            if (lit_len >= 15u) {
                size_t rem = lit_len - 15u;
                while (rem >= 255u) { *op++ = 255u; rem -= 255u; }
                *op++ = (uint8_t)rem;
            }
            if (lit_len > 0) {
                memcpy(op, anchor, lit_len);
                op += lit_len;
            }

            /* 16-bit LE match offset */
            op[0] = (uint8_t)(dist & 0xFFu);
            op[1] = (uint8_t)((dist >> 8) & 0xFFu);
            op += 2;

            if (m_extra >= 15u) {
                size_t rem = m_extra - 15u;
                while (rem >= 255u) { *op++ = 255u; rem -= 255u; }
                *op++ = (uint8_t)rem;
            }

            ip = m_ip;
            anchor = ip;
            if (ip < mflimit) {
                htab[lz4_hash4(read_u32_le(ip - 2))] = (uint32_t)(ip - 2 - src);
            }
        }
    }

    /* Emit final literal-only sequence */
    size_t last_lits = (size_t)(iend - anchor);
    size_t need = 1u + (last_lits / 255u) + 1u + last_lits;
    if ((size_t)(oend - op) < need) {
        free(htab);
        return 0;
    }
    uint8_t lit_tag = (last_lits >= 15u) ? 15u : (uint8_t)last_lits;
    *op++ = (uint8_t)(lit_tag << 4);
    if (last_lits >= 15u) {
        size_t rem = last_lits - 15u;
        while (rem >= 255u) { *op++ = 255u; rem -= 255u; }
        *op++ = (uint8_t)rem;
    }
    if (last_lits > 0) {
        memcpy(op, anchor, last_lits);
        op += last_lits;
    }

    free(htab);
    return (size_t)(op - dst);
}

/* Safe, bounds-checked LZ4 decompressor requiring exact output length. */
static bool lz4_decompress_exact(const uint8_t *src, size_t src_len,
                                 uint8_t *dst, size_t dst_len)
{
    const uint8_t *ip = src;
    const uint8_t *const iend = src + src_len;
    uint8_t *op = dst;
    uint8_t *const oend = dst + dst_len;

    while (ip < iend) {
        uint8_t token = *ip++;
        size_t lit_len = (size_t)(token >> 4);
        if (lit_len == 15u) {
            uint8_t s;
            do {
                if (ip >= iend) return false;
                s = *ip++;
                if (lit_len > dst_len - s) return false;
                lit_len += s;
            } while (s == 255u);
        }

        if ((size_t)(iend - ip) < lit_len || (size_t)(oend - op) < lit_len)
            return false;

        if (lit_len > 0) {
            memcpy(op, ip, lit_len);
            ip += lit_len;
            op += lit_len;
        }

        /* Trailing literal-only sequence ends the block cleanly */
        if (ip == iend) break;

        if ((size_t)(iend - ip) < 2u) return false;
        size_t offset = (size_t)ip[0] | ((size_t)ip[1] << 8);
        ip += 2;
        if (offset == 0 || offset > (size_t)(op - dst)) return false;

        size_t match_len = (size_t)(token & 0x0Fu);
        if (match_len == 15u) {
            uint8_t s;
            do {
                if (ip >= iend) return false;
                s = *ip++;
                if (match_len > dst_len - s) return false;
                match_len += s;
            } while (s == 255u);
        }
        match_len += LZ4_MIN_MATCH;
        if ((size_t)(oend - op) < match_len) return false;

        const uint8_t *ref = op - offset;
        if (offset == 1u) {
            /* Fast path for solid 4bpp tone runs (e.g. 0xFF white / 0x00 black) */
            memset(op, *ref, match_len);
            op += match_len;
        } else if (offset >= match_len) {
            memcpy(op, ref, match_len);
            op += match_len;
        } else {
            /* Overlapping periodic pattern (e.g. 936-byte vertical scanline
             * repeats or 4bpp halftone dither): seed `offset` bytes, then
             * exponentially double the already-materialized prefix via memcpy
             * so Xtensa vector memcpy runs instead of a byte-at-a-time loop. */
            size_t span = offset;
            memcpy(op, ref, span);
            size_t rem = match_len - span;
            uint8_t *cur = op + span;
            while (rem > 0) {
                size_t step = rem < span ? rem : span;
                memcpy(cur, op, step);
                cur += step;
                rem -= step;
                span += step;
            }
            op = cur;
        }
    }

    return ip == iend && op == oend;
}

bool frame_codec_encode(const uint8_t *raw, size_t raw_len,
                        uint8_t **out_blob, size_t *out_blob_len)
{
    if (!out_blob || !out_blob_len) return false;
    *out_blob = NULL;
    *out_blob_len = 0;
    if (!raw || raw_len == 0 || raw_len > UINT32_MAX - FRAME_CODEC_HEADER_BYTES)
        return false;

    /* Allocate room for header + raw_len; if LZ4 shrinks below raw_len, emit LZ4,
     * otherwise emit RAW in the exact same allocation with zero extra copy. */
    uint8_t *buf = codec_alloc(FRAME_CODEC_HEADER_BYTES + raw_len);
    if (!buf) return false;

    size_t comp_len = 0;
    if (raw_len > FRAME_CODEC_HEADER_BYTES + 16u) {
        comp_len = lz4_compress_bounded(raw, raw_len,
                                        buf + FRAME_CODEC_HEADER_BYTES,
                                        raw_len - FRAME_CODEC_HEADER_BYTES - 1u);
    }

    memcpy(buf, "TSZ1", 4);
    buf[4] = TSZ_VERSION;
    buf[6] = 0;
    buf[7] = 0;
    write_u32_le(buf + 8, (uint32_t)raw_len);

    if (comp_len > 0 && FRAME_CODEC_HEADER_BYTES + comp_len < raw_len) {
        buf[5] = TSZ_CODEC_LZ4;
        write_u32_le(buf + 12, (uint32_t)comp_len);
        *out_blob = buf;
        *out_blob_len = FRAME_CODEC_HEADER_BYTES + comp_len;
    } else {
        buf[5] = TSZ_CODEC_RAW;
        write_u32_le(buf + 12, (uint32_t)raw_len);
        memcpy(buf + FRAME_CODEC_HEADER_BYTES, raw, raw_len);
        *out_blob = buf;
        *out_blob_len = FRAME_CODEC_HEADER_BYTES + raw_len;
    }
    return true;
}

bool frame_codec_decode(const uint8_t *blob, size_t blob_len,
                        uint32_t expect_raw_bytes, uint8_t *dst_raw)
{
    if (!blob || !dst_raw || expect_raw_bytes == 0) return false;

    /* Every TSZ1 file is either strictly < expect_raw_bytes (LZ4) or
     * expect_raw_bytes + 16 (RAW), so exact expect_raw_bytes is unambiguously
     * a pre-existing unframed raw file regardless of its pixel bytes. */
    if (blob_len == (size_t)expect_raw_bytes) {
        memcpy(dst_raw, blob, expect_raw_bytes);
        return true;
    }

    if (blob_len >= FRAME_CODEC_HEADER_BYTES && memcmp(blob, "TSZ1", 4) == 0) {
        if (blob[4] != TSZ_VERSION || blob[6] != 0 || blob[7] != 0)
            return false;
        uint32_t unpacked_len = read_u32_le(blob + 8);
        uint32_t payload_len = read_u32_le(blob + 12);
        if (unpacked_len != expect_raw_bytes)
            return false;
        if (blob_len - FRAME_CODEC_HEADER_BYTES != (size_t)payload_len)
            return false;

        const uint8_t *payload = blob + FRAME_CODEC_HEADER_BYTES;
        if (blob[5] == TSZ_CODEC_RAW) {
            if (payload_len != expect_raw_bytes) return false;
            memcpy(dst_raw, payload, expect_raw_bytes);
            return true;
        }
        if (blob[5] == TSZ_CODEC_LZ4) {
            if (payload_len == 0) return false;
            return lz4_decompress_exact(payload, payload_len,
                                        dst_raw, expect_raw_bytes);
        }
        return false;
    }

    return false;
}
