#include "zc6.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/*
 * ============================================================================
 * OpenC6 ZC6 Payload Packaging & Compression Tool (CLI Utility)
 * Compress:   ./zc6_pack <input.bin> [output.zc6]
 * Decompress: ./zc6_pack -d <input.zc6> [output.bin]
 * ============================================================================
 */

extern bool hal_mem_load_file(const char *path, zc6_mem_buf_t *out_buf);
extern bool hal_mem_save_file(const char *path, const zc6_mem_buf_t *buf);

static void print_banner(void)
{
    printf("\033[1;34m====================================================================\033[0m\n");
    printf("\033[1;37m        OpenC6 ZC6 Payload Archiver & Compressor v4.0\033[0m\n");
    printf("\033[1;34m====================================================================\033[0m\n");
}

static void print_usage(const char *prog_name)
{
    print_banner();
    printf("Usage:\n");
    printf("  Compress:   %s <input.bin> [output.zc6]\n", prog_name);
    printf("  Decompress: %s -d <input.zc6> [output.bin]\n\n", prog_name);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    bool decompress_mode = false;
    const char *input_path = NULL;
    const char *output_path = NULL;
    char default_out_path[512];

    if (strcmp(argv[1], "-d") == 0) {
        decompress_mode = true;
        if (argc < 3) {
            print_usage(argv[0]);
            return 1;
        }
        input_path = argv[2];
        if (argc >= 4) {
            output_path = argv[3];
        } else {
            /* Strip .zc6 extension if present, or append .out */
            strncpy(default_out_path, input_path, sizeof(default_out_path) - 1);
            char *ext = strstr(default_out_path, ".zc6");
            if (ext) {
                *ext = '\0';
            } else {
                strncat(default_out_path, ".bin", sizeof(default_out_path) - strlen(default_out_path) - 1);
            }
            output_path = default_out_path;
        }
    } else {
        input_path = argv[1];
        if (argc >= 3) {
            output_path = argv[2];
        } else {
            snprintf(default_out_path, sizeof(default_out_path), "%s.zc6", input_path);
            output_path = default_out_path;
        }
    }

    print_banner();

    /* ─── 1. LOAD INPUT FILE ─────────────────────────────────────────────── */
    zc6_mem_buf_t in_buf;
    if (!hal_mem_load_file(input_path, &in_buf)) {
        printf("\033[1;31m[ERROR]\033[0m Cannot open input file: %s\n", input_path);
        return 1;
    }

    if (decompress_mode) {
        /* ─── DECOMPRESSION MODE ─────────────────────────────────────────── */
        printf("Mode         : \033[1;33mDECOMPRESS (-d)\033[0m\n");
        printf("Source File  : %s (%zu bytes)\n", input_path, in_buf.size);

        zc6_header_t hdr;
        if (!zc6_parse_header(in_buf.data, in_buf.size, &hdr)) {
            printf("\033[1;31m[ERROR]\033[0m Corrupted archive or invalid ZC6 magic header!\n");
            hal_mem_free(&in_buf);
            return 1;
        }

        zc6_mem_buf_t out_buf;
        if (!hal_mem_alloc(hdr.raw_size + 1024, &out_buf)) {
            printf("\033[1;31m[ERROR]\033[0m OOM allocating output memory!\n");
            hal_mem_free(&in_buf);
            return 1;
        }

        size_t decomp_len = 0;
        if (!zc6_decompress(in_buf.data, in_buf.size, out_buf.data, out_buf.capacity, &decomp_len)) {
            printf("\033[1;31m[ERROR]\033[0m Decompression failed! Stream truncated or CRC mismatch.\n");
            hal_mem_free(&in_buf);
            hal_mem_free(&out_buf);
            return 1;
        }
        out_buf.size = decomp_len;

        if (!hal_mem_save_file(output_path, &out_buf)) {
            printf("\033[1;31m[ERROR]\033[0m Failed writing output file: %s\n", output_path);
            hal_mem_free(&in_buf);
            hal_mem_free(&out_buf);
            return 1;
        }

        printf("Output File  : \033[1;32m%s\033[0m (%zu bytes)\n", output_path, out_buf.size);
        printf("Verification : \033[1;32m[CRC32 MATCH - OK]\033[0m\n");

        hal_mem_free(&in_buf);
        hal_mem_free(&out_buf);
    } else {
        /* ─── COMPRESSION MODE ───────────────────────────────────────────── */
        printf("Mode         : \033[1;36mCOMPRESS (ZC6 v4)\033[0m\n");
        printf("Source File  : %s (%zu bytes)\n", input_path, in_buf.size);

        zc6_mem_buf_t out_buf;
        size_t comp_cap = sizeof(zc6_header_t) + in_buf.size + 1024;
        if (!hal_mem_alloc(comp_cap, &out_buf)) {
            printf("\033[1;31m[ERROR]\033[0m OOM allocating compression buffer!\n");
            hal_mem_free(&in_buf);
            return 1;
        }

        if (!zc6_compress(in_buf.data, in_buf.size, &out_buf)) {
            printf("\033[1;31m[ERROR]\033[0m Compression engine failed!\n");
            hal_mem_free(&in_buf);
            hal_mem_free(&out_buf);
            return 1;
        }

        if (!hal_mem_save_file(output_path, &out_buf)) {
            printf("\033[1;31m[ERROR]\033[0m Failed writing output archive: %s\n", output_path);
            hal_mem_free(&in_buf);
            hal_mem_free(&out_buf);
            return 1;
        }

        double ratio = (1.0 - ((double)out_buf.size / (double)in_buf.size)) * 100.0;
        double factor = (double)in_buf.size / (double)out_buf.size;

        printf("Output File  : \033[1;32m%s\033[0m (%zu bytes)\n", output_path, out_buf.size);
        printf("Ratio        : \033[1;32m%.1f%% reduction\033[0m (Factor: \033[1;32m%.2fx\033[0m smaller)\n", ratio, factor);
        printf("Status       : \033[1;32m[ARCHIVED SUCCESSFULLY]\033[0m\n");

        hal_mem_free(&in_buf);
        hal_mem_free(&out_buf);
    }

    printf("\033[1;34m====================================================================\033[0m\n\n");
    return 0;
}
