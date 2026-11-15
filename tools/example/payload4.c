/**
 * @file payload4.c
 * @brief OpenC6 Microkernel OS: Filesystem ABI Test Payload.
 *
 * Tests fs_write_file, fs_read_file, and fs_delete via openc6_abi_t
 * from unprivileged RISC-V User Mode (U-Mode).
 */

#include "openc6_abi.h"

static void print_int32(const openc6_abi_t *abi, int32_t val) {
    char buf[16];
    int i = 14;
    int neg = 0;
    uint32_t uval;

    buf[15] = '\0';
    if (val < 0) {
        neg = 1;
        uval = (uint32_t)(-val);
    } else {
        uval = (uint32_t)val;
    }

    if (uval == 0) {
        buf[i--] = '0';
    } else {
        while (uval > 0 && i >= 0) {
            buf[i--] = '0' + (uval % 10);
            uval /= 10;
        }
    }

    if (neg && i >= 0) {
        buf[i--] = '-';
    }
    abi->print(&buf[i + 1]);
}

static uint32_t str_len(const char *s) {
    uint32_t len = 0;
    while (*s++) len++;
    return len;
}

static int buf_eq(const uint8_t *a, const uint8_t *b, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

void *memset(void *dest, int c, uint32_t n) {
    uint8_t *d = (uint8_t *)dest;
    while (n--) *d++ = (uint8_t)c;
    return dest;
}

void *memcpy(void *dest, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dest;
}

void __attribute__((section(".text.entry"), noreturn)) payload_main(const openc6_abi_t *abi) {
    if (!abi || abi->magic != OPENC6_ABI_MAGIC || abi->version != OPENC6_ABI_VERSION) {
        while (1) { __asm__ volatile("nop"); }
    }

    abi->print("\r\n==================================================\r\n");
    abi->print("     OPENC6 BIOS: FILESYSTEM ABI TEST (payload4)  \r\n");
    abi->print("==================================================\r\n\r\n");

    const char *fname = "abi_test.txt";
    const char *test_data = "OpenC6_FS_ABI_OK_12345";
    uint32_t data_len = str_len(test_data);
    uint8_t read_buf[64];
    memset(read_buf, 0, sizeof(read_buf));

    /* --- TEST 1: WRITE FILE --- */
    abi->print("[1] Calling abi->fs_write_file(\"abi_test.txt\", ...)...\r\n");
    abi->delay_ms(50);
    abi->fs_write_file(fname, (const uint8_t *)test_data, data_len, 0, 1);
    abi->print(" -> fs_write_file returned!\r\n\r\n");

    /* --- TEST 2: READ FILE & VERIFY --- */
    abi->print("[2] Calling abi->fs_read_file(\"abi_test.txt\", ...)...\r\n");
    abi->delay_ms(50);
    int32_t r_bytes = abi->fs_read_file(fname, read_buf, 0, data_len, 0);
    abi->print(" -> Read bytes: ");
    print_int32(abi, r_bytes);
    abi->print("\r\n");

    if (r_bytes == (int32_t)data_len && buf_eq(read_buf, (const uint8_t *)test_data, data_len)) {
        abi->print(" -> Data content: ");
        abi->print((const char *)read_buf);
        abi->print(" [MATCH OK]\r\n\r\n");
    } else {
        abi->print(" -> [FAIL] Data mismatch or read error!\r\n\r\n");
    }

    /* --- TEST 3: DELETE FILE & VERIFY --- */
    abi->print("[3] Calling abi->fs_delete(\"abi_test.txt\", 0)...\r\n");
    abi->delay_ms(50);
    abi->fs_delete(fname, 0);

    int32_t check_after = abi->fs_read_file(fname, read_buf, 0, data_len, 0);
    abi->print(" -> Read after delete: ");
    print_int32(abi, check_after);
    if (check_after < 0) {
        abi->print(" [DELETED OK]\r\n\r\n");
    } else {
        abi->print(" [FAIL: FILE STILL EXISTS]\r\n\r\n");
    }

    abi->print("[DONE] Filesystem ABI suite finished. Exiting cleanly...\r\n");
    abi->delay_ms(50);
    abi->sys_reset();
}
