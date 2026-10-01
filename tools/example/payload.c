/**
 * @file payload.c
 * @brief OpenC6 Microkernel OS: Bare-Metal Diagnostic & PMP Security Suite.
 *
 * Executes unprivileged U-mode system diagnostics including hardware TRNG,
 * cryptography, network socket binding, and deliberate PMP isolation testing.
 */

#include "openc6_abi.h"

/**
 * @brief Formats and prints an unsigned 32-bit integer as a decimal string via ABI.
 *
 * @param[in] abi Pointer to the OpenC6 ABI dispatch table.
 * @param[in] val Value to format and output.
 */
static void print_uint32(const openc6_abi_t *abi, uint32_t val) {
    char buf[16];
    int i = 14;
    buf[15] = '\0';

    if (val == 0) {
        buf[i--] = '0';
    } else {
        while (val > 0 && i >= 0) {
            buf[i--] = '0' + (val % 10);
            val /= 10;
        }
    }
    abi->print(&buf[i + 1]);
}

/**
 * @brief Formats and prints an 8-bit byte as a 2-character hexadecimal string.
 *
 * @param[in] abi Pointer to the OpenC6 ABI dispatch table.
 * @param[in] val 8-bit value to display.
 */
static void print_hex_byte(const openc6_abi_t *abi, uint8_t val) {
    char buf[3];
    uint8_t hi = val >> 4;
    uint8_t lo = val & 0x0F;
    buf[0] = hi < 10 ? '0' + hi : 'A' + hi - 10;
    buf[1] = lo < 10 ? '0' + lo : 'A' + lo - 10;
    buf[2] = '\0';
    abi->print(buf);
}

/**
 * @brief Formats and prints a 32-bit address or value in canonical hexadecimal format (0xXXXXXXXX).
 *
 * @param[in] abi Pointer to the OpenC6 ABI dispatch table.
 * @param[in] val 32-bit unsigned integer or address.
 */
static void print_hex(const openc6_abi_t *abi, uint32_t val) {
    char buf[11];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 7; i >= 0; i--) {
        uint8_t nibble = (val >> (i * 4)) & 0x0F;
        buf[9 - i] = (nibble < 10) ? ('0' + nibble) : ('A' + nibble - 10);
    }
    buf[10] = '\0';
    abi->print(buf);
}

/**
 * @brief Standard bare-metal byte memory initialization.
 *
 * @param[out] dest Target buffer pointer.
 * @param[in]  c    Value to set.
 * @param[in]  n    Number of bytes to write.
 * @return Pointer to memory area dest.
 */
void *memset(void *dest, int c, uint32_t n) {
    uint8_t *d = (uint8_t *)dest;
    while (n--) *d++ = (uint8_t)c;
    return dest;
}

/**
 * @brief Standard bare-metal memory block copy.
 *
 * @param[out] dest Destination buffer.
 * @param[in]  src  Source buffer.
 * @param[in]  n    Number of bytes to copy.
 * @return Pointer to destination buffer.
 */
void *memcpy(void *dest, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dest;
}

/**
 * @brief Calculates length of a null-terminated string.
 *
 * @param[in] s Pointer to string.
 * @return Character count excluding null terminator.
 */
static uint32_t str_len(const char *s) {
    uint32_t len = 0;
    while (*s++) len++;
    return len;
}

/**
 * @brief Main payload entry point executed in unprivileged RISC-V User Mode (U-Mode).
 *
 * @param[in] abi Pointer to system ABI dispatch function matrix.
 */
void __attribute__((section(".text.entry"), noreturn)) payload_main(const openc6_abi_t *abi) {

    /* --- STEP 0: ABI SANITY VALIDATION --- */
    /* Validate structural boundary integrity before dereferencing function pointers */
    if (!abi || abi->magic != OPENC6_ABI_MAGIC || abi->version != OPENC6_ABI_VERSION) {
        while (1) { __asm__ volatile("nop"); }
    }

    abi->print("\r\n==================================================\r\n");
    abi->print("     OPENC6 BIOS: COMPLETE SYSTEM & SECURITY SUITE \r\n");
    abi->print("==================================================\r\n\r\n");

    /* --- TEST 1: SYSTEM MEMORY METRICS --- */
    abi->print("[1] SYSTEM MEMORY TEST\r\n");
    uint32_t flash_kb = abi->get_total_flash() / 1024;
    uint32_t ram_tot_kb = abi->get_total_ram() / 1024;
    uint32_t ram_free_kb = abi->get_free_ram() / 1024;

    abi->print(" -> Total Flash: "); print_uint32(abi, flash_kb); abi->print(" KB\r\n");
    abi->print(" -> Total RAM:   "); print_uint32(abi, ram_tot_kb); abi->print(" KB\r\n");
    abi->print(" -> Free RAM:    "); print_uint32(abi, ram_free_kb); abi->print(" KB\r\n\r\n");

    /* --- TEST 2: HEAP Dynamic Allocation --- */
    abi->print("[2] DYNAMIC ALLOCATION TEST\r\n");
    uint8_t *test_buf = (uint8_t*)abi->malloc(1024);
    if (test_buf) {
        abi->print(" -> Malloc (1024 bytes): OK!\r\n");
        test_buf[0] = 0xAA;
        abi->free(test_buf);
        abi->print(" -> Memory Freed: OK!\r\n\r\n");
    } else {
        abi->print(" -> Malloc FAILED!\r\n\r\n");
    }

    /* --- TEST 3: HARDWARE CRYPTO & TRNG --- */
    abi->print("[3] HARDWARE CRYPTO & TRNG TEST\r\n");
    abi->print(" -> TRNG Num 1: "); print_uint32(abi, abi->get_random()); abi->print("\r\n");
    abi->print(" -> TRNG Num 2: "); print_uint32(abi, abi->get_random()); abi->print("\r\n");

    const char* secret = "OpenC6";
    uint8_t hash[32];
    abi->sha256((const uint8_t*)secret, 6, hash);
    abi->print(" -> SHA-256 ('OpenC6'): ");
    for (int i = 0; i < 32; i++) {
        print_hex_byte(abi, hash[i]);
    }
    abi->print("\r\n\r\n");

    /* --- TEST 4: MATHEMATICAL FIXED-POINT ACCELERATOR --- */
    abi->print("[4] MATH ACCELERATION TEST\r\n");
    abi->print(" -> isqrt(144) = "); print_uint32(abi, abi->math_isqrt(144)); abi->print("\r\n");
    abi->print(" -> sin(90 deg) = "); print_uint32(abi, (uint32_t)abi->math_sin_deg(90)); abi->print(" (x10000)\r\n\r\n");

    /* --- TEST 5: SOCKET INTERFACE & EMBEDDED HTTP SERVER --- */
    abi->print("[5] NETWORK & EMBEDDED WEB SERVER TEST\r\n");

    if (abi->wifi_is_connected()) {
        char ip_str[32];
        memset(ip_str, 0, sizeof(ip_str));
        abi->net_get_ip(ip_str, sizeof(ip_str));

        abi->print(" -> [ONLINE] Wi-Fi Link is Active!\r\n");
        abi->print(" -> Node IP Address: "); abi->print(ip_str); abi->print("\r\n");

        int32_t server_fd = abi->tcp_listen(443);
        if (server_fd >= 0) {
            abi->print(" -> HTTP Server listening on http://"); abi->print(ip_str); abi->print(":443/\r\n");
            abi->print(" -> Server is active for 30 seconds. Connect via browser!\r\n");

            uint32_t elapsed = 0;
            while (elapsed < 30000) {
                int32_t client_fd = abi->tcp_accept(server_fd, 500);
                if (client_fd >= 0) {
                    char req[128];
                    abi->tcp_read(client_fd, req, sizeof(req) - 1);

                    const char *http_response =
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/html; charset=utf-8\r\n"
                    "Connection: close\r\n\r\n"
                    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                    "<title>OpenC6 Microkernel OS</title></head>"
                    "<body style='background:#0d1117;color:#58a6ff;font-family:monospace;padding:30px;line-height:1.6;'>"
                    "<h1 style='color:#39d353;'>OpenC6 Micro Web Server</h1>"
                    "<p style='color:#f0f6fc;font-size:16px;'>Status: <b>RUNNING UNDER HARDWARE PMP ISOLATION!</b></p>"
                    "<div style='background:#161b22;padding:15px;border-radius:8px;border:1px solid #30363d;'>"
                    "<p style='margin:5px 0;'>CPU Core   : <b>RISC-V RV32IMAC (ESP32-C6)</b></p>"
                    "<p style='margin:5px 0;'>Privilege  : <b>User Mode (U-Mode)</b></p>"
                    "<p style='margin:5px 0;'>Protection : <b>Physical Memory Protection (PMP TOR)</b></p>"
                    "<p style='margin:5px 0;'>Security   : <b>Kernel Space Shielded (SRAM & MMIO)</b></p>"
                    "</div>"
                    "<hr style='border:0;border-top:1px solid #30363d;margin:20px 0;'>"
                    "<p style='color:#8b949e;font-size:12px;'>Served live by an unprivileged isolated process via OpenC6 Sockets ABI.</p>"
                    "</body></html>";

                    abi->tcp_write(client_fd, http_response, str_len(http_response));

                    /* Delay to allow MAC/PHY layer enough time to drain socket buffers */
                    abi->delay_ms(60);

                    abi->tcp_close(client_fd);
                    abi->print(" -> [HTTP] Web page served to client!\r\n");
                }
                elapsed += 500;
            }
            abi->tcp_close(server_fd);
            abi->print(" -> HTTP Server session ended.\r\n");
        } else {
            abi->print(" -> [ERROR] Failed to bind TCP port 80!\r\n");
        }
    } else {
        abi->print(" -> [OFFLINE] Wi-Fi station not connected.\r\n");
    }
    abi->print("\r\n");

    /* --- TEST 6: GPIO & PERIPHERAL ACTUATION --- */
    abi->print("[6] HARDWARE CONTROL & DELAY TEST\r\n");
    abi->print(" -> Executing RGB sequence (3 seconds)...\r\n");

    for (int i = 0; i < 3; i++) {
        abi->set_led_color(255, 0, 0); abi->delay_ms(333);
        abi->set_led_color(0, 255, 0); abi->delay_ms(333);
        abi->set_led_color(0, 0, 255); abi->delay_ms(333);
    }
    abi->set_led_color(0, 0, 0);
    abi->print(" -> RGB Sequence: OK!\r\n\r\n");

    /* --- TEST 7: VALID U-MODE MEMORY ACCESS --- */
    abi->print("[7] PMP VALID ACCESS TEST (Inside dynamic arena)\r\n");
    volatile uint32_t local_var = 0x12345678;
    local_var ^= 0xA5A5A5A5;
    abi->print(" -> Stack variable write/read: ");
    print_hex(abi, local_var);
    abi->print(" [OK]\r\n\r\n");

    /* --- TEST 8: PMP HARDWARE BOUNDARY VIOLATION --- */
    /*
     * Address 0x40800000 points to kernel vector table in protected SRAM.
     * Enforced by PMP Entry 4 (Permissions: NONE for U-Mode).
     * Any U-Mode store to this range MUST trigger a RISC-V Store Access Fault.
     */
    uint32_t target_addr = 0x40800000;

    abi->print("[8] PMP SECURITY ATTACK: ATTEMPTING MALICIOUS WRITE TO KERNEL SRAM!\r\n");
    abi->print(" -> Target Address : "); print_hex(abi, target_addr); abi->print("\r\n");
    abi->print(" -> Payload Value  : 0xDEADBEEF\r\n");
    abi->print(" -> Executing store instruction directly from unprivileged U-Mode...\r\n");

    /* Allow UART FIFO buffer to flush prior to exception generation */
    abi->delay_ms(100);

    /*
     * CRITICAL HARDWARE SECURITY TRIGGER:
     * This instruction causes RISC-V mcause = 7 (Store Access Fault).
     * Kernel exception handler intercepts exception and terminates process safely.
     */
    *((volatile uint32_t *)target_addr) = 0xDEADBEEF;

    /* --- UNREACHABLE CODE PATH --- */
    /* Triggered only if PMP hardware protection fails or is disabled */
    abi->set_led_color(255, 0, 0);
    abi->print("\r\n[CRITICAL FAILURE] PMP DID NOT TRIGGER! MEMORY CORRUPTED!\r\n");

    while (1) {
        abi->delay_ms(1000);
    }
}
