#include "zc6.h"
#include <string.h>

/*
 * ============================================================================
 * OpenC6 ZC6 v4 In-Place Streaming Decompressor
 * Strictly zero dynamic heap allocation.
 * Features 1-Byte TinyMatch, Short/Long Matches, RLE, and Sparse RV32 Words.
 * ============================================================================
 */

/**
 * @brief Computes standard IEEE 802.3 CRC32 checksum for payload verification.
 */
static uint32_t zc6_decompress_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFUL;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320UL & (-(crc & 1UL)));
        }
    }
    return ~crc;
}

bool zc6_decompress(const uint8_t *src, size_t src_len,
                    uint8_t *dst, size_t dst_capacity,
                    size_t *out_decompressed_len)
{
    if (!src || src_len < sizeof(zc6_header_t) || !dst || dst_capacity == 0) {
        return false;
    }

    /* ─── 1. PARSE AND VALIDATE ZC6 V4 HEADER ────────────────────────────── */
    zc6_header_t hdr;
    if (!zc6_parse_header(src, src_len, &hdr)) {
        return false;
    }

    if (hdr.raw_size > dst_capacity) {
        return false; /* Destination page pool overflow */
    }

    size_t comp_end = sizeof(zc6_header_t) + hdr.comp_size;
    if (comp_end > src_len) {
        return false; /* Truncated archive stream */
    }

    size_t src_idx = sizeof(zc6_header_t);
    size_t dst_idx = 0;

    /* ─── 2. STREAMING DECOMPRESSION ENGINE ──────────────────────────────── */
    while (src_idx < comp_end && dst_idx < hdr.raw_size) {
        uint8_t tag = src[src_idx++];
        uint8_t opcode = tag & ZC6_OP_MASK;
        uint8_t param = tag & ZC6_PAYLOAD_MASK;

        switch (opcode) {
            /* ─── CASE A: TINY MATCH (1-BYTE TOKEN! Dist <= 16, Len 3..6) ── */
            case ZC6_OP_TINY_MATCH: {
                size_t len = ((param >> 4) & 0x03) + ZC6_TINY_LEN_MIN;
                uint16_t dist = (param & 0x0F) + 1;

                if (dist == 0 || dist > dst_idx || dst_idx + len > dst_capacity) {
                    return false;
                }

                hal_mem_copy_window(&dst[dst_idx], &dst[dst_idx - dist], len);
                dst_idx += len;
                break;
            }

            /* ─── CASE B: SHORT MATCH (2-BYTE TOKEN, Dist <= 255) ────────── */
            case ZC6_OP_SHORT_MATCH: {
                size_t len = param + ZC6_MIN_MATCH_LEN;

                if (src_idx >= comp_end) return false;
                uint16_t dist = (uint16_t)src[src_idx++];

                if (dist == 0 || dist > dst_idx || dst_idx + len > dst_capacity) {
                    return false;
                }

                hal_mem_copy_window(&dst[dst_idx], &dst[dst_idx - dist], len);
                dst_idx += len;
                break;
            }

            /* ─── CASE C: LONG MATCH (3-BYTE TOKEN, Dist > 255) ─────────── */
            case ZC6_OP_LONG_MATCH: {
                size_t len = param + ZC6_MIN_MATCH_LEN;

                if (src_idx + 2 > comp_end) return false;
                uint16_t dist = (uint16_t)src[src_idx] | ((uint16_t)src[src_idx + 1] << 8);
                src_idx += 2;

                if (dist == 0 || dist > dst_idx || dst_idx + len > dst_capacity) {
                    return false;
                }

                hal_mem_copy_window(&dst[dst_idx], &dst[dst_idx - dist], len);
                dst_idx += len;
                break;
            }

            /* ─── CASE D: SPECIAL MODES (Literals / Zero / RLE / Sparse) ─── */
            case ZC6_OP_SPECIAL: {
                if (param <= 31) {
                    /* Sub-mode 0: Raw Literals (1..32 bytes) */
                    size_t count = param + 1;
                    if (src_idx + count > comp_end || dst_idx + count > dst_capacity) {
                        return false;
                    }

                    memcpy(&dst[dst_idx], &src[src_idx], count);
                    src_idx += count;
                    dst_idx += count;
                } else if (param == ZC6_SPECIAL_ZERO_RUN) {
                    /* Sub-mode 1: Multi-word zero sequence (1..255 words) */
                    if (src_idx >= comp_end) return false;
                    size_t zero_words = src[src_idx++];
                    size_t zero_bytes = zero_words * 4;

                    if (dst_idx + zero_bytes > dst_capacity) return false;

                    memset(&dst[dst_idx], 0, zero_bytes);
                    dst_idx += zero_bytes;
                } else if (param == ZC6_SPECIAL_RLE_LONG) {
                    /* Sub-mode 2: Long Byte RLE (1..255 bytes) */
                    if (src_idx + 2 > comp_end) return false;
                    size_t run_len = src[src_idx++];
                    uint8_t val = src[src_idx++];

                    if (dst_idx + run_len > dst_capacity) return false;

                    memset(&dst[dst_idx], val, run_len);
                    dst_idx += run_len;
                } else if (param >= ZC6_SPECIAL_SPARSE_BASE && param < ZC6_SPECIAL_RLE_SHORT) {
                    /* Sub-mode 3: Sparse 32-bit RV32 Word (4-bit mask 1..14) */
                    uint8_t mask = (param - ZC6_SPECIAL_SPARSE_BASE) + 1;
                    uint32_t word = 0;

                    if (mask & 1) {
                        if (src_idx >= comp_end) return false;
                        word |= (uint32_t)src[src_idx++];
                    }
                    if (mask & 2) {
                        if (src_idx >= comp_end) return false;
                        word |= ((uint32_t)src[src_idx++]) << 8;
                    }
                    if (mask & 4) {
                        if (src_idx >= comp_end) return false;
                        word |= ((uint32_t)src[src_idx++]) << 16;
                    }
                    if (mask & 8) {
                        if (src_idx >= comp_end) return false;
                        word |= ((uint32_t)src[src_idx++]) << 24;
                    }

                    if (dst_idx + 4 > dst_capacity) return false;

                    if (((uintptr_t)&dst[dst_idx] & 3) == 0) {
                        hal_mem_write_word32(&dst[dst_idx], word);
                    } else {
                        dst[dst_idx + 0] = (uint8_t)(word);
                        dst[dst_idx + 1] = (uint8_t)(word >> 8);
                        dst[dst_idx + 2] = (uint8_t)(word >> 16);
                        dst[dst_idx + 3] = (uint8_t)(word >> 24);
                    }
                    dst_idx += 4;
                } else if (param >= ZC6_SPECIAL_RLE_SHORT) {
                    /* Sub-mode 4: Short Byte RLE (4..19 bytes, 2-byte token) */
                    size_t run_len = (param - ZC6_SPECIAL_RLE_SHORT) + 4;
                    if (src_idx >= comp_end) return false;
                    uint8_t val = src[src_idx++];

                    if (dst_idx + run_len > dst_capacity) return false;

                    memset(&dst[dst_idx], val, run_len);
                    dst_idx += run_len;
                } else {
                    return false; /* Reserved */
                }
                break;
            }

            default:
                return false;
        }
    }

    /* ─── 3. INTEGRITY & DATA VERIFICATION ───────────────────────────────── */
    if (dst_idx != hdr.raw_size) {
        return false; /* Restored size mismatch */
    }

    uint32_t calc_crc = zc6_decompress_crc32(dst, dst_idx);
    if (calc_crc != hdr.checksum) {
        return false; /* Data corruption detected */
    }

    if (out_decompressed_len) {
        *out_decompressed_len = dst_idx;
    }

    return true;
}
