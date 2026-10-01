#include "zc6.h"
#include <string.h>
#include <stdlib.h>

/*
 * ============================================================================
 * OpenC6 ZC6-Lite Embedded Payload Compressor (Microkernel ZSWAP Engine)
 * Low-footprint match finder (~1.2 KB RAM during compression) for ESP32-C6.
 * ============================================================================
 */

#define ZC6_HASH_SIZE            256U
#define ZC6_HASH_MASK            (ZC6_HASH_SIZE - 1U)
#define ZC6_HASH_WAYS            2U

typedef struct {
    uint16_t entries[ZC6_HASH_SIZE][ZC6_HASH_WAYS];
    uint8_t  head[ZC6_HASH_SIZE];
} zc6_match_finder_t;

/**
 * @brief Computes standard IEEE 802.3 CRC32 checksum.
 */
static uint32_t zc6_crc32(const uint8_t *data, size_t len)
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

/**
 * @brief Flushes accumulated raw literals into stream using ZC6_OP_SPECIAL (0xC0).
 * Maximum literal chunk size is 32 bytes (params 0x00..0x1F).
 */
static size_t flush_literals(uint8_t *dst, size_t dst_cap, size_t dst_idx,
                             const uint8_t *lit_src, size_t count,
                             void *unused __attribute__((unused)))
{
    size_t written = 0;
    size_t offset = 0;

    while (offset < count) {
        size_t chunk = (count - offset > ZC6_SPECIAL_LITERAL_MAX) ? ZC6_SPECIAL_LITERAL_MAX : (count - offset);
        if (dst_idx + written + 1 + chunk > dst_cap) {
            return 0; /* Overflow */
        }

        dst[dst_idx + written] = (uint8_t)(ZC6_OP_SPECIAL | (chunk - 1));
        written++;

        memcpy(&dst[dst_idx + written], &lit_src[offset], chunk);
        written += chunk;

        offset += chunk;
    }

    return written;
}

static inline void mf_insert(zc6_match_finder_t *mf, const uint8_t *src, size_t src_len, size_t pos)
{
    if (pos + ZC6_MIN_MATCH_LEN <= src_len) {
        uint32_t h = ((src[pos] << 8) ^ (src[pos + 1] << 4) ^ src[pos + 2]) & ZC6_HASH_MASK;
        uint8_t slot = mf->head[h];
        mf->entries[h][slot] = (uint16_t)(pos + 1);
        mf->head[h] = (slot + 1) % ZC6_HASH_WAYS;
    }
}

static inline void find_best_match(zc6_match_finder_t *mf, const uint8_t *src, size_t src_len,
                                   size_t pos, size_t *out_len, size_t *out_dist)
{
    *out_len = 0;
    *out_dist = 0;

    if (pos + ZC6_MIN_MATCH_LEN > src_len) return;

    uint32_t h = ((src[pos] << 8) ^ (src[pos + 1] << 4) ^ src[pos + 2]) & ZC6_HASH_MASK;

    for (size_t w = 0; w < ZC6_HASH_WAYS; w++) {
        uint16_t cand_pos = mf->entries[h][w];
        if (cand_pos == 0) continue;

        size_t cand_idx = cand_pos - 1;
        size_t dist = pos - cand_idx;

        if (dist > 0 && dist <= ZC6_MAX_WINDOW_SIZE) {
            size_t mlen = 0;
            while ((pos + mlen < src_len) &&
                (mlen < ZC6_MAX_MATCH_LEN) &&
                (src[pos + mlen] == src[cand_idx + mlen])) {
                mlen++;
                }
                if (mlen > *out_len) {
                    *out_len = mlen;
                    *out_dist = dist;
                    if (*out_len == ZC6_MAX_MATCH_LEN) break;
                }
        }
    }
}

bool zc6_compress(const uint8_t *src, size_t src_len, zc6_mem_buf_t *out_compressed)
{
    if (!src || src_len == 0 || !out_compressed || !out_compressed->data) {
        return false;
    }

    size_t dst_cap = out_compressed->capacity;
    if (dst_cap < (sizeof(zc6_header_t) + src_len + 128)) {
        return false;
    }

    uint8_t *dst = out_compressed->data;
    size_t dst_idx = sizeof(zc6_header_t);

    zc6_match_finder_t *mf = (zc6_match_finder_t *)calloc(1, sizeof(zc6_match_finder_t));
    if (!mf) {
        return false;
    }

    size_t src_idx = 0;
    size_t lit_start = 0;
    size_t lit_count = 0;

    while (src_idx < src_len) {
        /* ─── 1. DETECT BYTE-LEVEL RUN-LENGTH ENCODING (RLE) ─────────────── */
        size_t rle_len = 0;
        uint8_t rle_val = src[src_idx];
        while (src_idx + rle_len < src_len && src[src_idx + rle_len] == rle_val && rle_len < 255) {
            rle_len++;
        }
        int rle_savings = 0;
        if (rle_len >= 4) {
            rle_savings = (rle_len <= 19) ? ((int)rle_len - 2) : ((int)rle_len - 3);
        }

        /* ─── 2. DETECT MULTI-WORD ZERO SEQUENCES (32-bit Zero Run) ──────── */
        size_t zero_words = 0;
        if (src_idx + 4 <= src_len &&
            src[src_idx] == 0 && src[src_idx+1] == 0 && src[src_idx+2] == 0 && src[src_idx+3] == 0) {

            size_t probe = src_idx;
        while ((probe + 4 <= src_len) && zero_words < 255) {
            if (src[probe] == 0 && src[probe+1] == 0 && src[probe+2] == 0 && src[probe+3] == 0) {
                zero_words++;
                probe += 4;
            } else {
                break;
            }
        }
            }
            int zero_savings = (zero_words > 0) ? ((int)zero_words * 4 - 2) : 0;

            /* ─── 3. DETECT TINYLZ MATCH (Tiny 1B, Short 2B, Long 3B) ────────── */
            size_t best_len = 0;
            size_t best_dist = 0;
            find_best_match(mf, src, src_len, src_idx, &best_len, &best_dist);

            int lz_savings = 0;
            bool use_tiny = false;

            if (best_dist <= ZC6_TINY_DIST_MAX && best_len >= ZC6_TINY_LEN_MIN) {
                size_t tlen = (best_len > ZC6_TINY_LEN_MAX) ? ZC6_TINY_LEN_MAX : best_len;
                int tiny_sav = (int)tlen - 1;
                int short_sav = (int)best_len - 2;

                if (short_sav > tiny_sav) {
                    lz_savings = short_sav;
                    use_tiny = false;
                } else {
                    lz_savings = tiny_sav;
                    best_len = tlen;
                    use_tiny = true;
                }
            } else if (best_dist <= ZC6_SHORT_DIST_MAX && best_len >= ZC6_MIN_MATCH_LEN) {
                lz_savings = (int)best_len - 2;
                use_tiny = false;
            } else if (best_dist > ZC6_SHORT_DIST_MAX && best_len >= 4) {
                lz_savings = (int)best_len - 3;
                use_tiny = false;
            } else {
                best_len = 0;
            }

            /* Lazy evaluation: probe next position for superior match */
            if (lz_savings > 0 && best_len < ZC6_MAX_MATCH_LEN && (src_idx + 1 < src_len)) {
                size_t next_len = 0, next_dist = 0;
                find_best_match(mf, src, src_len, src_idx + 1, &next_len, &next_dist);

                int next_sav = 0;
                if (next_dist <= ZC6_TINY_DIST_MAX && next_len >= ZC6_TINY_LEN_MIN) {
                    size_t ntlen = (next_len > ZC6_TINY_LEN_MAX) ? ZC6_TINY_LEN_MAX : next_len;
                    int tsav = (int)ntlen - 1;
                    int ssav = (int)next_len - 2;
                    next_sav = (ssav > tsav) ? ssav : tsav;
                } else if (next_dist <= ZC6_SHORT_DIST_MAX && next_len >= ZC6_MIN_MATCH_LEN) {
                    next_sav = (int)next_len - 2;
                } else if (next_dist > ZC6_SHORT_DIST_MAX && next_len >= 4) {
                    next_sav = (int)next_len - 3;
                }

                if (next_sav > lz_savings + 1) {
                    mf_insert(mf, src, src_len, src_idx);
                    lit_count++;
                    src_idx++;
                    continue;
                }
            }

            mf_insert(mf, src, src_len, src_idx);

            /* ─── 4. DETECT SPARSE 32-BIT WORDS ──────────────────────────────── */
            uint8_t sparse_mask = 0;
            int non_zero_count = 4;
            if (src_idx + 4 <= src_len && zero_words == 0) {
                uint8_t b0 = src[src_idx];
                uint8_t b1 = src[src_idx + 1];
                uint8_t b2 = src[src_idx + 2];
                uint8_t b3 = src[src_idx + 3];

                sparse_mask = (b0 ? 1 : 0) | (b1 ? 2 : 0) | (b2 ? 4 : 0) | (b3 ? 8 : 0);
                non_zero_count = (b0 ? 1 : 0) + (b1 ? 1 : 0) + (b2 ? 1 : 0) + (b3 ? 1 : 0);
            }
            int sparse_savings = (non_zero_count <= 2 && sparse_mask > 0 && sparse_mask < 15) ?
            (4 - (1 + non_zero_count)) : 0;

            /* ─── 5. COST-BENEFIT ARBITRATION ────────────────────────────────── */

            /* A. Take Byte RLE */
            if (rle_savings > 0 && rle_savings >= lz_savings && rle_savings >= zero_savings) {
                if (lit_count > 0) {
                    size_t fl = flush_literals(dst, dst_cap, dst_idx, &src[lit_start], lit_count, NULL);
                    if (fl == 0) { free(mf); return false; }
                    dst_idx += fl;
                    lit_count = 0;
                }

                if (rle_len <= 19) {
                    dst[dst_idx++] = (uint8_t)(ZC6_OP_SPECIAL | (ZC6_SPECIAL_RLE_SHORT + (rle_len - 4)));
                    dst[dst_idx++] = rle_val;
                } else {
                    dst[dst_idx++] = (uint8_t)(ZC6_OP_SPECIAL | ZC6_SPECIAL_RLE_LONG);
                    dst[dst_idx++] = (uint8_t)rle_len;
                    dst[dst_idx++] = rle_val;
                }

                for (size_t k = 1; k < rle_len; k++) {
                    mf_insert(mf, src, src_len, src_idx + k);
                }

                src_idx += rle_len;
                lit_start = src_idx;
                continue;
            }

            /* B. Take Zero-Word Run */
            if (zero_savings > 0 && zero_savings >= lz_savings) {
                if (lit_count > 0) {
                    size_t fl = flush_literals(dst, dst_cap, dst_idx, &src[lit_start], lit_count, NULL);
                    if (fl == 0) { free(mf); return false; }
                    dst_idx += fl;
                    lit_count = 0;
                }

                dst[dst_idx++] = (uint8_t)(ZC6_OP_SPECIAL | ZC6_SPECIAL_ZERO_RUN);
                dst[dst_idx++] = (uint8_t)zero_words;

                for (size_t k = 1; k < zero_words * 4; k++) {
                    mf_insert(mf, src, src_len, src_idx + k);
                }

                src_idx += (zero_words * 4);
                lit_start = src_idx;
                continue;
            }

            /* C. Take TinyLZ Match (Tiny 1B, Short 2B, or Long 3B) */
            if (lz_savings > 0 && lz_savings >= sparse_savings) {
                if (lit_count > 0) {
                    size_t fl = flush_literals(dst, dst_cap, dst_idx, &src[lit_start], lit_count, NULL);
                    if (fl == 0) { free(mf); return false; }
                    dst_idx += fl;
                    lit_count = 0;
                }

                if (use_tiny) {
                    uint8_t len_bits = (uint8_t)((best_len - ZC6_TINY_LEN_MIN) & 0x03);
                    uint8_t dist_bits = (uint8_t)((best_dist - 1) & 0x0F);
                    dst[dst_idx++] = (uint8_t)(ZC6_OP_TINY_MATCH | (len_bits << 4) | dist_bits);
                } else if (best_dist <= ZC6_SHORT_DIST_MAX) {
                    dst[dst_idx++] = (uint8_t)(ZC6_OP_SHORT_MATCH | (best_len - ZC6_MIN_MATCH_LEN));
                    dst[dst_idx++] = (uint8_t)best_dist;
                } else {
                    dst[dst_idx++] = (uint8_t)(ZC6_OP_LONG_MATCH | (best_len - ZC6_MIN_MATCH_LEN));
                    dst[dst_idx++] = (uint8_t)(best_dist & 0xFF);
                    dst[dst_idx++] = (uint8_t)((best_dist >> 8) & 0xFF);
                }

                for (size_t k = 1; k < best_len; k++) {
                    mf_insert(mf, src, src_len, src_idx + k);
                }

                src_idx += best_len;
                lit_start = src_idx;
                continue;
            }

            /* D. Take Sparse 32-bit Word */
            if (sparse_savings > 0) {
                if (lit_count > 0) {
                    size_t fl = flush_literals(dst, dst_cap, dst_idx, &src[lit_start], lit_count, NULL);
                    if (fl == 0) { free(mf); return false; }
                    dst_idx += fl;
                    lit_count = 0;
                }

                dst[dst_idx++] = (uint8_t)(ZC6_OP_SPECIAL | (ZC6_SPECIAL_SPARSE_BASE + (sparse_mask - 1)));
                if (sparse_mask & 1) dst[dst_idx++] = src[src_idx];
                if (sparse_mask & 2) dst[dst_idx++] = src[src_idx + 1];
                if (sparse_mask & 4) dst[dst_idx++] = src[src_idx + 2];
                if (sparse_mask & 8) dst[dst_idx++] = src[src_idx + 3];

                for (size_t k = 1; k < 4; k++) {
                    mf_insert(mf, src, src_len, src_idx + k);
                }

                src_idx += 4;
                lit_start = src_idx;
                continue;
            }

            /* E. Accumulate Literal Byte */
            lit_count++;
            src_idx++;
    }

    if (lit_count > 0) {
        size_t fl = flush_literals(dst, dst_cap, dst_idx, &src[lit_start], lit_count, NULL);
        if (fl == 0) { free(mf); return false; }
        dst_idx += fl;
    }

    free(mf);

    /* ─── 6. FINALIZE CANONICAL ZC6 V4 HEADER ────────────────────────────── */
    zc6_header_t hdr;
    hdr.magic = ZC6_MAGIC;
    hdr.version = ZC6_VERSION;
    hdr.flags = 0;
    hdr.raw_size = (uint32_t)src_len;
    hdr.comp_size = (uint32_t)(dst_idx - sizeof(zc6_header_t));
    hdr.checksum = zc6_crc32(src, src_len);

    memcpy(dst, &hdr, sizeof(zc6_header_t));
    out_compressed->size = dst_idx;

    return true;
}
