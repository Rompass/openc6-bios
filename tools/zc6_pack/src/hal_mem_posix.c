#define _POSIX_C_SOURCE 200809L
#include "hal_mem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * ============================================================================
 * OpenC6 ZC6 Memory HAL - Linux POSIX Implementation
 * Simulates page-allocated embedded SRAM and provides disk I/O test fixtures.
 * ============================================================================
 */

bool hal_mem_alloc(size_t capacity, zc6_mem_buf_t *out_buf)
{
    if (!out_buf || capacity == 0) {
        return false;
    }

    /* Enforce 16-byte alignment to mirror RISC-V RV32 calling conventions and cache lines */
    void *ptr = NULL;
    int res = posix_memalign(&ptr, 16, capacity);
    if (res != 0 || !ptr) {
        return false;
    }

    memset(ptr, 0, capacity);
    out_buf->data = (uint8_t *)ptr;
    out_buf->size = 0;
    out_buf->capacity = capacity;
    return true;
}

void hal_mem_free(zc6_mem_buf_t *buf)
{
    if (!buf) {
        return;
    }
    if (buf->data) {
        free(buf->data);
        buf->data = NULL;
    }
    buf->size = 0;
    buf->capacity = 0;
}

/**
 * @brief Testbench fixture: Loads a raw binary payload from disk into virtual RAM.
 *
 * @param path File system path to the payload binary.
 * @param out_buf Destination memory buffer.
 * @return true on success, false on read error or OOM.
 */
bool hal_mem_load_file(const char *path, zc6_mem_buf_t *out_buf)
{
    if (!path || !out_buf) {
        return false;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0) {
        fclose(f);
        return false;
    }

    if (!hal_mem_alloc((size_t)sz, out_buf)) {
        fclose(f);
        return false;
    }

    size_t read_bytes = fread(out_buf->data, 1, (size_t)sz, f);
    fclose(f);

    if (read_bytes != (size_t)sz) {
        hal_mem_free(out_buf);
        return false;
    }

    out_buf->size = read_bytes;
    return true;
}

/**
 * @brief Testbench fixture: Writes virtual memory buffer contents to disk.
 *
 * @param path Destination file system path.
 * @param buf Source memory buffer.
 * @return true on success, false on write error.
 */
bool hal_mem_save_file(const char *path, const zc6_mem_buf_t *buf)
{
    if (!path || !buf || !buf->data || buf->size == 0) {
        return false;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }

    size_t written = fwrite(buf->data, 1, buf->size, f);
    fclose(f);

    return (written == buf->size);
}
