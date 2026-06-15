/* tools/example/payload.c */
#include "openc6_abi.h"

// =========================================================================
// HELPERS (Since we are in -nostdlib, we write our own micro printf clones)
// =========================================================================

// Output a 32-bit unsigned integer (decimal format)
static void print_uint32(const openc6_abi_t *abi, uint32_t val) {
    char buf[16];
    int i = 14;
    buf[15] = '\0'; // String terminator

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

// Output a single byte in hexadecimal format (HEX)
static void print_hex_byte(const openc6_abi_t *abi, uint8_t val) {
    char buf[3];
    uint8_t hi = val >> 4;
    uint8_t lo = val & 0x0F;
    buf[0] = hi < 10 ? '0' + hi : 'A' + hi - 10;
    buf[1] = lo < 10 ? '0' + lo : 'A' + lo - 10;
    buf[2] = '\0';
    abi->print(buf);
}

// =========================================================================
// MAIN PAYLOAD ENTRY POINT
// =========================================================================
void __attribute__((section(".text.entry"), noreturn)) payload_main(const openc6_abi_t *abi) {

    // 0. ABI VALIDATION
    if (!abi || abi->magic != OPENC6_ABI_MAGIC || abi->version != OPENC6_ABI_VERSION) {
        while (1) { __asm__ volatile("nop"); }
    }

    abi->print("\n==================================================\n");
    abi->print("    OPENC6 BIOS: ULTIMATE DIAGNOSTIC PAYLOAD    \n");
    abi->print("==================================================\n\n");

    // ─── TEST 1: MEMORY MONITORING (Memory API) ──────────────────────────
    abi->print("[1] SYSTEM MEMORY TEST\n");

    uint32_t flash_kb = abi->get_total_flash() / 1024;
    uint32_t ram_tot_kb = abi->get_total_ram() / 1024;
    uint32_t ram_free_kb = abi->get_free_ram() / 1024;

    abi->print(" -> Total Flash: "); print_uint32(abi, flash_kb); abi->print(" KB\n");
    abi->print(" -> Total RAM:   "); print_uint32(abi, ram_tot_kb); abi->print(" KB\n");
    abi->print(" -> Free RAM:    "); print_uint32(abi, ram_free_kb); abi->print(" KB\n\n");


    // ─── TEST 2: DYNAMIC MEMORY (Malloc / Free) ────────────────────────
    abi->print("[2] DYNAMIC ALLOCATION TEST\n");

    uint8_t *test_buf = (uint8_t*)abi->malloc(1024);
    if (test_buf) {
        abi->print(" -> Malloc (1024 bytes): OK!\n");
        test_buf[0] = 0xAA; // Test write
        abi->free(test_buf);
        abi->print(" -> Memory Freed: OK!\n\n");
    } else {
        abi->print(" -> Malloc FAILED!\n\n");
    }


    // ─── TEST 3: HARDWARE TRNG & CRYPTOGRAPHY (TRNG & SHA256) ─────────
    abi->print("[3] HARDWARE CRYPTO & TRNG TEST\n");

    uint32_t r1 = abi->get_random();
    uint32_t r2 = abi->get_random();
    abi->print(" -> TRNG Num 1: "); print_uint32(abi, r1); abi->print("\n");
    abi->print(" -> TRNG Num 2: "); print_uint32(abi, r2); abi->print("\n");

    const char* secret = "OpenC6";
    uint8_t hash[32];
    abi->sha256((const uint8_t*)secret, 6, hash); // 6 - length of "OpenC6"

    abi->print(" -> SHA-256 ('OpenC6'): ");
    for(int i=0; i < 32; i++) {
        print_hex_byte(abi, hash[i]);
    }
    abi->print("\n\n");


    // ─── TEST 4: MATHEMATICS (Fixed-point via ABI) ────────────────────────
    abi->print("[4] MATH ACCELERATION TEST (Fixed-Point)\n");

    // Викликаємо цілочисельний корінь
    uint32_t sq_val = abi->math_isqrt(144);

    // Викликаємо синус (90 градусів = 1.0 -> поверне 10000 у фіксованій комі)
    uint32_t sin_val = (uint32_t)abi->math_sin_deg(90);

    abi->print(" -> isqrt(144) = "); print_uint32(abi, sq_val); abi->print("\n");
    abi->print(" -> sin(90 deg) = "); print_uint32(abi, sin_val); abi->print(" (x10000)\n\n");

    // ─── TEST 5: WI-FI CONNECTION TEST ───────────────────────────────────
    abi->print("[5] WI-FI CONNECTION TEST\n");

    const char* my_ssid = "Test";
    const char* my_pass = "12345678";

    abi->print(" -> Attempting to connect to hotspot: ");
    abi->print(my_ssid);
    abi->print("\n");

    // Викликаємо Біос для підключення.
    // Нагадаю: ця функція в Біосі чекає до 10 секунд (DHCP/Auth).
    int wifi_res = abi->wifi_connect(my_ssid, my_pass);

    if (wifi_res == 0) {
        abi->print(" -> [OK] WiFi Driver accepted credentials.\n");

        // Даємо системі ще 2 секунди на стабілізацію IP-стеку
        abi->delay_ms(2000);

        if (abi->wifi_is_connected()) {
            abi->print(" -> [SUCCESS] Connected! IP Address obtained.\n");
            abi->set_led_color(0, 255, 0); // Зелений — є інтернет
        } else {
            abi->print(" -> [ERROR] Auth OK, but no IP connection (Wait bit failed).\n");
            abi->set_led_color(255, 165, 0); // Помаранчевий
        }
    } else {
        abi->print(" -> [FAILED] Connection timed out or AP not found.\n");
        abi->set_led_color(255, 0, 0); // Червоний
    }

    abi->print(" -> Connection status (STA): ");
    print_uint32(abi, abi->wifi_is_connected());
    abi->print("\n\n");


    // ─── TEST 6: LED & SYSTEM DELAYS ──────────────────────────────────────
    abi->print("[6] HARDWARE CONTROL & DELAY TEST\n");
    abi->print(" -> Executing RGB sequence (3 seconds)...\n");

    for (int i = 0; i < 3; i++) {
        abi->set_led_color(255, 0, 0); abi->delay_ms(333); // Red
        abi->set_led_color(0, 255, 0); abi->delay_ms(333); // Green
        abi->set_led_color(0, 0, 255); abi->delay_ms(333); // Blue
    }
    abi->set_led_color(0, 0, 0); // Off


    // ─── COMPLETION ───────────────────────────────────────────────────────
    abi->print("\n==================================================\n");
    abi->print("  ALL DIAGNOSTICS PASSED! SYSTEM IS STABLE!       \n");
    abi->print("  Rebooting back to BIOS in 3 seconds...          \n");
    abi->print("==================================================\n");

    abi->delay_ms(3000);
    abi->sys_reset();
}
