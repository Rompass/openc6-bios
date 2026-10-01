#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * ============================================================================
 * OpenC6 ZC6 Compression Subsystem - Memory Abstraction Layer (HAL)
 * Provides platform-independent memory stream operations.
 * Allows identical compression logic to compile on Linux testbench and ESP32.
 * ============================================================================
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Virtual memory buffer representation */
typedef struct {
    uint8_t *data;          /* Base pointer to raw memory block */
    size_t   size;          /* Current valid byte count */
    size_t   capacity;      /* Total allocated memory boundary */
} zc6_mem_buf_t;

/**
 * @brief Allocates an aligned memory buffer.
 * On Linux: Uses standard heap/posix_memalign.
 * On ESP32: Maps directly to proc_mempool_alloc() pages.
 *
 * @param capacity Maximum byte capacity to allocate.
 * @param out_buf Pointer to destination buffer descriptor.
 * @return true on allocation success, false on OOM.
 */
bool hal_mem_alloc(size_t capacity, zc6_mem_buf_t *out_buf);

/**
 * @brief Releases a previously allocated memory buffer.
 *
 * @param buf Pointer to buffer descriptor to free.
 */
void hal_mem_free(zc6_mem_buf_t *buf);

/**
 * @brief Fast aligned 32-bit word store operation.
 * Key primitive for Hybrid Sparse-Bitmask decompression.
 *
 * @param dst Destination address (must be 4-byte aligned).
 * @param word 32-bit value to store.
 */
static inline void hal_mem_write_word32(uint8_t *dst, uint32_t word)
{
    *(volatile uint32_t *)dst = word;
}

/**
 * @brief Fast aligned 32-bit word load operation.
 *
 * @param src Source address (must be 4-byte aligned).
 * @return 32-bit loaded value.
 */
static inline uint32_t hal_mem_read_word32(const uint8_t *src)
{
    return *(const volatile uint32_t *)src;
}

/**
 * @brief Fast byte-copy utility for overlapping LZ dictionary windows.
 *
 * @param dst Target destination memory pointer.
 * @param src Source window memory pointer (may overlap with dst).
 * @param len Number of bytes to replicate.
 */
static inline void hal_mem_copy_window(uint8_t *dst, const uint8_t *src, size_t len)
{
    while (len--) {
        *dst++ = *src++;
    }
}

#ifdef __cplusplus
}
#endif
