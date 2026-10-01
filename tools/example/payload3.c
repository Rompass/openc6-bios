/* tools/example/payload3.c
 * OpenC6 Microkernel OS: Hardware GPIO & Security Validation Suite
 * Zero-rodata implementation to avoid absolute addressing faults.
 */
#include "openc6_abi.h"

#define GPIO_TEST_PIN       7

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

static inline void test_single_pin(const openc6_abi_t *abi, uint32_t pin, uint32_t *passed) {
    int32_t dir_res = abi->gpio_set_dir(pin, 1);
    int32_t write_res = abi->gpio_write(pin, 1);

    abi->print(" -> Pin ");
    print_uint32(abi, pin);
    abi->print(" Access: ");

    if (dir_res == -1 && write_res == -1) {
        abi->print("[BLOCKED / SAFE]\r\n");
        (*passed)++;
    } else {
        abi->print("[SECURITY BREACH!]\r\n");
    }
}

void __attribute__((section(".text.entry"), noreturn)) payload_main(const openc6_abi_t *abi) {

    if (!abi || abi->magic != OPENC6_ABI_MAGIC || abi->version != OPENC6_ABI_VERSION) {
        while (1) { __asm__ volatile("nop"); }
    }

    abi->print("\r\n==================================================\r\n");
    abi->print("     OPENC6 GPIO & PIN SECURITY TEST SUITE       \r\n");
    abi->print("==================================================\r\n\r\n");

    /* ─── PART 1: KERNEL PIN PROTECTION AUDIT (Stack-only execution) ────── */
    abi->print("[1] TESTING HARDWARE PIN SECURITY FILTER:\r\n");

    uint32_t passed = 0;

    test_single_pin(abi, 1,  &passed); /* CMOS GND */
    test_single_pin(abi, 2,  &passed); /* CMOS Sense */
    test_single_pin(abi, 3,  &passed); /* ME GND */
    test_single_pin(abi, 4,  &passed); /* ME Sense */
    test_single_pin(abi, 8,  &passed); /* WS2812 LED */
    test_single_pin(abi, 9,  &passed); /* BOOT Pin */
    test_single_pin(abi, 12, &passed); /* USB D- */
    test_single_pin(abi, 13, &passed); /* USB D+ */
    test_single_pin(abi, 25, &passed); /* SPI Flash */
    test_single_pin(abi, 31, &passed); /* Out of range */

    abi->print("\r\nSecurity Result: ");
    print_uint32(abi, passed);
    abi->print("/10 protected pins securely shielded.\r\n\r\n");

    /* ─── PART 2: HARDWARE MULTIMETER VOLTAGE TOGGLE ─────────────────────── */
    abi->print("[2] TESTING ACCESSIBLE USER GPIO (Multimeter Probe Test)\r\n");
    abi->print(" -> Target Test Pin: GPIO ");
    print_uint32(abi, GPIO_TEST_PIN);
    abi->print("\r\n -> Connect multimeter between GPIO ");
    print_uint32(abi, GPIO_TEST_PIN);
    abi->print(" and GND.\r\n");
    abi->print(" -> Toggling every 2000 ms...\r\n\r\n");

    int32_t init_res = abi->gpio_set_dir(GPIO_TEST_PIN, 1);
    if (init_res != 0) {
        abi->print("[ERROR] Failed to configure GPIO as output!\r\n");
        while (1) { abi->delay_ms(1000); }
    }

    uint32_t cycle = 1;

    while (1) {
        /* HIGH (3.3V) */
        abi->gpio_write(GPIO_TEST_PIN, 1);
        int32_t state_high = abi->gpio_read(GPIO_TEST_PIN);

        abi->print("[Cycle ");
        print_uint32(abi, cycle);
        abi->print("] GPIO ");
        print_uint32(abi, GPIO_TEST_PIN);
        abi->print(" -> HIGH (3.3V) | Readback: ");
        print_uint32(abi, (uint32_t)state_high);
        abi->print("\r\n");

        abi->delay_ms(2000);

        /* LOW (0.0V) */
        abi->gpio_write(GPIO_TEST_PIN, 0);
        int32_t state_low = abi->gpio_read(GPIO_TEST_PIN);

        abi->print("[Cycle ");
        print_uint32(abi, cycle);
        abi->print("] GPIO ");
        print_uint32(abi, GPIO_TEST_PIN);
        abi->print(" -> LOW  (0.0V) | Readback: ");
        print_uint32(abi, (uint32_t)state_low);
        abi->print("\r\n");

        abi->delay_ms(2000);
        cycle++;
    }
}
