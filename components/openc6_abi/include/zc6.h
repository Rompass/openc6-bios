#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
/* Virtual memory buffer representation */
typedef struct {
    uint8_t *data;          /* Base pointer to raw memory block */
    size_t   size;          /* Current valid byte count */
    size_t   capacity;      /* Total allocated memory boundary */
} zc6_mem_buf_t;

/**
 * @brief Fast aligned 32-bit word store operation.
 */
static inline void hal_mem_write_word32(uint8_t *dst, uint32_t word)
{
    *(volatile uint32_t *)dst = word;
}

/**
 * @brief Fast byte-copy utility for overlapping LZ dictionary windows.
 */
static inline void hal_mem_copy_window(uint8_t *dst, const uint8_t *src, size_t len)
{
    while (len--) {
        *dst++ = *src++;
    }
}

/*
 * ============================================================================
 * OpenC6 ZC6 v4 Architecture Specification (1-Byte TinyMatch Engine)
 * Tiny Match (1B) + Short Match (2B) + Long Match (3B) + Special/RLE/Sparse.
 * ============================================================================
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Magic identifier "!ZC6" in little-endian byte order (0x21, 'Z', 'C', '6') */
#define ZC6_MAGIC                0x36435A21UL
#define ZC6_VERSION              4U

/* Sliding window boundaries matching 4 KB OpenC6 page allocator */
#define ZC6_MAX_WINDOW_SIZE      4096U
#define ZC6_TINY_DIST_MAX        16U   /* 4 bits: 0..15 -> dist 1..16 */
#define ZC6_TINY_LEN_MIN         3U
#define ZC6_TINY_LEN_MAX         6U    /* 2 bits: 0..3 -> len 3..6 */

#define ZC6_SHORT_DIST_MAX       255U
#define ZC6_MIN_MATCH_LEN        3U
#define ZC6_MAX_MATCH_LEN        66U   /* 6 bits: 0..63 + MIN_MATCH_LEN */

/* 2-bit Primary Command Tag (bits 7:6) */
#define ZC6_OP_MASK              0xC0U
#define ZC6_OP_TINY_MATCH        0x00U /* 1-Byte Token: Dist <= 16, Len 3..6 (0 extra bytes!) */
#define ZC6_OP_SHORT_MATCH       0x40U /* 2-Byte Token: Dist <= 255, Len 3..66 (1 byte dist) */
#define ZC6_OP_LONG_MATCH        0x80U /* 3-Byte Token: Dist > 255, Len 3..66 (2 bytes dist) */
#define ZC6_OP_SPECIAL           0xC0U /* Literals / Zero Run / RLE / Sparse Words */

#define ZC6_PAYLOAD_MASK         0x3FU /* Lower 6 bits for opcode parameters (0..63) */

/* Sub-modes for ZC6_OP_SPECIAL (bits 5:0) */
#define ZC6_SPECIAL_LITERAL_MAX  32U   /* 0x00..0x1F: Raw literals (1..32 bytes) */
#define ZC6_SPECIAL_ZERO_RUN     0x20U /* Multi-word zero sequence (1..255 words) */
#define ZC6_SPECIAL_RLE_LONG     0x21U /* Byte RLE with external length byte (1..255 bytes) */
#define ZC6_SPECIAL_SPARSE_BASE  0x22U /* 0x22..0x2F: Sparse 32-bit Word (mask 1..14) */
#define ZC6_SPECIAL_RLE_SHORT    0x30U /* 0x30..0x3F: Short Byte RLE (lengths 4..19, 2B token!) */

#pragma pack(push, 1)

/**
 * @brief Canonical 20-byte file header for .zc6 v4 compressed payloads.
 */
typedef struct {
    uint32_t magic;                 /* Must match ZC6_MAGIC */
    uint16_t version;               /* ZC6 format version (v4) */
    uint16_t flags;                 /* Reserved / feature flags */
    uint32_t raw_size;              /* Uncompressed binary image size in bytes */
    uint32_t comp_size;             /* Compressed payload byte count (excluding header) */
    uint32_t checksum;              /* CRC32 / integrity checksum of raw image */
} zc6_header_t;

#pragma pack(pop)

/**
 * @brief Compresses a raw RISC-V binary payload using ZC6 v4 encoding.
 *
 * @param src Pointer to uncompressed binary payload data.
 * @param src_len Length of raw input data in bytes.
 * @param out_compressed Destination buffer descriptor allocated by caller.
 * @return true on compression success, false on buffer overflow or invalid arguments.
 */
bool zc6_compress(const uint8_t *src, size_t src_len, zc6_mem_buf_t *out_compressed);

/**
 * @brief In-Place Streaming Decompressor for ZC6 v4 archives.
 * Strictly 0 bytes dynamic RAM overhead.
 *
 * @param src Pointer to compressed archive (starting at zc6_header_t).
 * @param src_len Total length of compressed archive.
 * @param dst Pointer to destination memory buffer (RAM page).
 * @param dst_capacity Capacity of destination memory block.
 * @param out_decompressed_len Output parameter receiving actual restored byte count.
 * @return true on success, false on corruption, CRC mismatch or capacity overflow.
 */
bool zc6_decompress(const uint8_t *src, size_t src_len,
                    uint8_t *dst, size_t dst_capacity,
                    size_t *out_decompressed_len);

/**
 * @brief Validates ZC6 archive header and extracts uncompressed size.
 *
 * @param src Pointer to archive buffer.
 * @param src_len Archive length.
 * @param out_hdr Output descriptor receiving parsed header fields.
 * @return true if header magic and version are valid, false otherwise.
 */
bool zc6_parse_header(const uint8_t *src, size_t src_len, zc6_header_t *out_hdr);

#ifdef __cplusplus
}
#endif
