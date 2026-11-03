/*
 * frame_codec.h: Fast LZ4 frame compression envelope for cached Base Frames.
 *
 * On-disk format ("TSZ1" 16-byte header):
 *   [0..3]   magic "TSZ1"
 *   [4]      format version (1)
 *   [5]      codec: 0 = RAW, 1 = LZ4 block
 *   [6..7]   reserved (0)
 *   [8..11]  unpacked_len (uint32 LE)
 *   [12..15] payload_len  (uint32 LE)
 *   [16..]   payload bytes
 *
 * Legacy unframed files whose byte length equals expect_raw_bytes and do not
 * start with "TSZ1" are copied through raw so pre-existing SD caches remain
 * readable; callers still verify the unpacked SHA-256 digest afterward.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FRAME_CODEC_HEADER_BYTES 16u

/* Compress `raw[0..raw_len)` into a newly allocated TSZ1 envelope (`*out_blob`,
 * `*out_blob_len`). Falls back to a RAW TSZ1 envelope when incompressible so
 * worst-case output never exceeds `raw_len + 16`. Caller free()s `*out_blob`. */
bool frame_codec_encode(const uint8_t *raw, size_t raw_len,
                        uint8_t **out_blob, size_t *out_blob_len);

/* Decode `blob[0..blob_len)` into caller-allocated `dst_raw` of capacity at
 * least `expect_raw_bytes`. Returns true iff the decoded output is valid and
 * produces `expect_raw_bytes` bytes exactly. */
bool frame_codec_decode(const uint8_t *blob, size_t blob_len,
                        uint32_t expect_raw_bytes, uint8_t *dst_raw);
