#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "rom/ets_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_ota_ops.h"
#include "esp_flash.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "soc/soc.h"
#include "soc/wdev_reg.h"
#include "driver/rtc_io.h"

#include "bios_core.h"
#include "hw_usb.h"
#include "sandbox.h"
#include "nvram.h"
#include "led_mgmt.h"
#include "wifi_mgmt.h"
#include "web_ui.h"
#include "boot_manager.h"
#include "power_tweaker.h"
#include "management_engine.h"
#include "me_shared.h"
#include "pxe_boot.h"
#include "openc6_fs.h"
#include "hal_flash.h"
#include "openc6_syscall.h"

#define BIOS_AP_SSID    "BIOS_SETUP_C6"
#define PIN_BTN_GND     3
#define PIN_BTN_SENSE   4
#define PIN_BTN_BOOT    9

static const char *TAG = "BIOS_CORE";

/* Shared atomic spinlock guarding bare-metal USB-Serial-JTAG endpoint FIFOs */
volatile uint32_t g_hw_usb_lock = 0;
/* Active console telemetry tap hook for c6wsh web socket/chunked stream */
volatile hw_usb_tap_fn_t g_hw_usb_tap = NULL;

bool is_me_enabled = true;

typedef void (*abi_sys_reset_fn)(void) __attribute__((noreturn));

/**
 * @brief Intercepts standard ESP_LOG output and redirects characters to bare-metal USB registers.
 * Avoids FreeRTOS VFS driver overhead during early boot and trap contexts.
 *
 * @param fmt Format string.
 * @param args Variable argument list.
 * @return Total number of characters transmitted.
 */
static int bios_vprintf(const char *fmt, va_list args)
{
    char buf[256];
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    if (len > 0) {
        hw_usb_print(buf);
    }
    return len;
}

/**
 * @brief Linker Wrap: Overrides ESP-IDF's aggressive PMP region locking.
 * Prevents the IDF startup code from setting the permanent PMP_L lock bit on SRAM,
 * ensuring OpenC6 dynamic sandbox retains 100% hardware authority over PMP registers!
 */
void __wrap_esp_cpu_configure_region_protection(void)
{
    /* Intentionally empty! Do NOT allow ESP-IDF to lock PMP slots with PMP_L! */
}

/* Real ESP-IDF hardware restart function provided by linker */
extern void __real_esp_restart_noos(void);

/**
 * @brief Linker Wrap: Intercepts CPU reset call (esp_restart_noos).
 * Differentiates between authentic hardware exceptions and clean software restarts.
 * Dumps registers only on genuine CPU traps, then safely hands off to real reset logic.
 */
void __wrap_esp_restart_noos(void)
{
    uint32_t mcause = 0;
    asm volatile ("csrr %0, 0x342" : "=r"(mcause));

    /* Check if this is an actual CPU hardware exception (bit 31 is 0 and cause is non-zero) */
    bool is_genuine_crash = ((mcause & (1UL << 31)) == 0) && (mcause != 0);

    if (is_genuine_crash) {
        uint32_t mepc = 0, mtval = 0, ra = 0, sp = 0;
        asm volatile ("csrr %0, 0x341" : "=r"(mepc));
        asm volatile ("csrr %0, 0x343" : "=r"(mtval));
        asm volatile ("mv %0, ra" : "=r"(ra));
        asm volatile ("mv %0, sp" : "=r"(sp));

        hw_usb_print("\r\n======================================================================\r\n");
        hw_usb_print("[CRASH_INTERCEPTED] CPU Exception trapped before reboot!\r\n");
        hw_usb_print("======================================================================\r\n");

        char buf[96];
        snprintf(buf, sizeof(buf), "  Faulting PC (MEPC)   : 0x%08lx\r\n", (unsigned long)mepc);
        hw_usb_print(buf);

        snprintf(buf, sizeof(buf), "  Caller RA   (Return) : 0x%08lx\r\n", (unsigned long)ra);
        hw_usb_print(buf);

        snprintf(buf, sizeof(buf), "  Trap Cause  (MCAUSE) : 0x%08lx (Code: %lu)\r\n",
                 (unsigned long)mcause, (unsigned long)(mcause & 0x7FFFFFFFUL));
        hw_usb_print(buf);

        snprintf(buf, sizeof(buf), "  Target Addr (MTVAL)  : 0x%08lx\r\n", (unsigned long)mtval);
        hw_usb_print(buf);

        snprintf(buf, sizeof(buf), "  Stack Ptr   (SP)     : 0x%08lx\r\n", (unsigned long)sp);
        hw_usb_print(buf);

        hw_usb_print("======================================================================\r\n\r\n");

        /* Brief delay allowing USB FIFO to drain telemetry buffer to PC */
        for (volatile int i = 0; i < 2000000; i++) {
            __asm__ volatile("nop");
        }
    }

    /* Hand off to original hardware restart routine */
    __real_esp_restart_noos();
}

/**
 * @brief Busy-wait millisecond delay utilizing 64-bit hardware microsecond timer.
 * Bypasses vTaskDelay() dependency on FreeRTOS SysTick timer when interrupts are masked.
 * Actively feeds the LP-Core Management Engine watchdog to prevent reset during execution.
 *
 * @param ms Delay duration in milliseconds.
 */
static void bios_delay_ms(uint32_t ms)
{
    register uint32_t a7 asm("a7") = SYS_DELAY_MS;
    register uint32_t a0 asm("a0") = ms;
    asm volatile (
        "ecall"
        : "+r"(a0)
        : "r"(a7)
        : "memory"
    );
}

/**
 * @brief Allocates dynamic memory within active sandbox arena or host RTOS heap.
 * Prevents privilege faults on mstatus spinlocks when invoked from U-mode sandbox.
 *
 * @param size Allocation size in bytes.
 * @return Pointer to allocated memory block, or NULL on OOM.
 */
static void* bios_malloc(uint32_t size)
{
    register uint32_t a7 asm("a7") = SYS_MALLOC;
    register uint32_t a0 asm("a0") = size;
    asm volatile ("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return (void *)a0;
}

/**
 * @brief Releases dynamic memory back to sandbox free-list or host heap.
 *
 * @param ptr Pointer previously returned by bios_malloc.
 */
static void bios_free(void *ptr)
{
    register uint32_t a7 asm("a7") = SYS_FREE;
    register uint32_t a0 asm("a0") = (uint32_t)ptr;
    asm volatile ("ecall" : "+r"(a0) : "r"(a7) : "memory");
}

/**
 * @brief Terminates payload execution or initiates cold SoC reboot.
 * Intercepts calls originating within U-mode sandbox to cleanly restore shell context
 * without dropping the physical USB CDC connection to the host terminal.
 */
static void bios_sys_reset(void) __attribute__((noreturn));

static void bios_sys_reset(void)
{
    register uint32_t a7 asm("a7") = SYS_SYS_RESET;
    asm volatile ("ecall" : : "r"(a7) : "memory");
    while (1) {}
}

static void bios_print(const char *str)
{
    register uint32_t a7 asm("a7") = SYS_PRINT;
    register uint32_t a0 asm("a0") = (uint32_t)str;
    asm volatile ("ecall" : "+r"(a0) : "r"(a7) : "memory");
}

/**
 * @brief Reads hardware true random number directly from ESP32-C6 silicon TRNG.
 * ESP32-C6 TRM Rev 1.2: WDEV_RND_REG samples high-speed thermal analog noise from RF PHY.
 * Completely independent of FreeRTOS scheduler critical sections.
 *
 * @return 32-bit hardware entropy value.
 */
static uint32_t bios_get_random(void)
{
    register uint32_t a7 asm("a7") = SYS_GET_RANDOM;
    register uint32_t a0 asm("a0");
    asm volatile ("ecall" : "=r"(a0) : "r"(a7) : "memory");
    return a0;
}

/**
 * @brief Standalone SHA-256 implementation independent of PSA Crypto and dynamic heap.
 * Executed in Machine Mode via trap dispatcher to avoid unprivileged IRAM execution faults.
 *
 * @param input Pointer to source data buffer.
 * @param len Byte length of input data.
 * @param output Destination buffer for 32-byte hash digest.
 */
void bios_sha256_compute(const uint8_t *input, uint32_t len, uint8_t *output)
{
    uint32_t h[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };

    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    };

    uint8_t chunk[64];
    uint64_t total_bits = (uint64_t)len * 8ULL;
    uint32_t offset = 0;

    while (offset < len) {
        uint32_t chunk_len = len - offset;
        if (chunk_len > 64) chunk_len = 64;
        memcpy(chunk, input + offset, chunk_len);

        if (chunk_len < 64) {
            chunk[chunk_len] = 0x80;
            memset(chunk + chunk_len + 1, 0, 63 - chunk_len);
            if (chunk_len < 56) {
                for (int i = 0; i < 8; i++) chunk[63 - i] = (uint8_t)(total_bits >> (i * 8));
            }
        }

        uint32_t w[64];
        for (int t = 0; t < 16; t++) {
            w[t] = ((uint32_t)chunk[t * 4] << 24) |
            ((uint32_t)chunk[t * 4 + 1] << 16) |
            ((uint32_t)chunk[t * 4 + 2] << 8) |
            ((uint32_t)chunk[t * 4 + 3]);
        }
        for (int t = 16; t < 64; t++) {
            uint32_t s0 = (((w[t-15] >> 7) | (w[t-15] << 25)) ^ ((w[t-15] >> 18) | (w[t-15] << 14)) ^ (w[t-15] >> 3));
            uint32_t s1 = (((w[t-2] >> 17) | (w[t-2] << 15)) ^ ((w[t-2] >> 19) | (w[t-2] << 13)) ^ (w[t-2] >> 10));
            w[t] = w[t-16] + s0 + w[t-7] + s1;
        }

        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], h_val = h[7];
        for (int t = 0; t < 64; t++) {
            uint32_t S1 = ((e >> 6) | (e << 26)) ^ ((e >> 11) | (e << 21)) ^ ((e >> 25) | (e << 7));
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t temp1 = h_val + S1 + ch + k[t] + w[t];
            uint32_t S0 = ((a >> 2) | (a << 30)) ^ ((a >> 13) | (a << 19)) ^ ((a >> 22) | (a << 10));
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t temp2 = S0 + maj;

            h_val = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }

        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += h_val;

        offset += chunk_len;
        if (chunk_len == 64 && offset == len) {
            memset(chunk, 0, 64);
            chunk[0] = 0x80;
            for (int i = 0; i < 8; i++) chunk[63 - i] = (uint8_t)(total_bits >> (i * 8));
            offset--;
        }
    }

    for (int i = 0; i < 8; i++) {
        output[i * 4]     = (uint8_t)(h[i] >> 24);
        output[i * 4 + 1] = (uint8_t)(h[i] >> 16);
        output[i * 4 + 2] = (uint8_t)(h[i] >> 8);
        output[i * 4 + 3] = (uint8_t)(h[i]);
    }
}

static void bios_sha256(const uint8_t *input, uint32_t len, uint8_t *output)
{
    register uint32_t a7 asm("a7") = SYS_SHA256;
    register uint32_t a0 asm("a0") = (uint32_t)input;
    register uint32_t a1 asm("a1") = len;
    register uint32_t a2 asm("a2") = (uint32_t)output;
    asm volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
}

static uint32_t bios_math_isqrt(uint32_t x)
{
    register uint32_t a7 asm("a7") = SYS_MATH_ISQRT;
    register uint32_t a0 asm("a0") = x;
    asm volatile ("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return a0;
}

static int32_t bios_math_sin_deg(int32_t deg)
{
    register uint32_t a7 asm("a7") = SYS_MATH_SIN_DEG;
    register uint32_t a0 asm("a0") = (uint32_t)deg;
    asm volatile ("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return (int32_t)a0;
}

static int32_t bios_math_cos_deg(int32_t deg)
{
    register uint32_t a7 asm("a7") = SYS_MATH_COS_DEG;
    register uint32_t a0 asm("a0") = (uint32_t)deg;
    asm volatile ("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return (int32_t)a0;
}

/**
 * @brief Queries station IP acquisition status across isolation boundary.
 *
 * @return 1 if station has acquired an IP address, 0 otherwise.
 */
static int32_t bios_wifi_is_connected(void)
{
    register uint32_t a7 asm("a7") = SYS_WIFI_IS_CONNECTED;
    register uint32_t a0 asm("a0");

    asm volatile (
        "ecall"
        : "=r"(a0)
        : "r"(a7)
        : "memory"
    );
    return (int32_t)a0;
}

static uint32_t bios_get_free_ram(void)
{
    register uint32_t a7 asm("a7") = SYS_GET_FREE_RAM;
    register uint32_t a0 asm("a0");
    asm volatile ("ecall" : "=r"(a0) : "r"(a7) : "memory");
    return a0;
}

static uint32_t bios_get_total_ram(void)
{
    register uint32_t a7 asm("a7") = SYS_GET_TOTAL_RAM;
    register uint32_t a0 asm("a0");
    asm volatile ("ecall" : "=r"(a0) : "r"(a7) : "memory");
    return a0;
}

static uint32_t bios_get_total_flash(void)
{
    register uint32_t a7 asm("a7") = SYS_GET_TOTAL_FLASH;
    register uint32_t a0 asm("a0");
    asm volatile ("ecall" : "=r"(a0) : "r"(a7) : "memory");
    return a0;
}

static void abi_fs_write_file(const char *name, const uint8_t *data, uint32_t len, uint32_t parent_id, uint8_t force)
{
    fs_write_file(name, data, len, (uint16_t)parent_id);
}

static int32_t abi_fs_read_file(const char *name, uint8_t *dest, uint32_t offset, uint32_t len, uint32_t parent_id)
{
    int16_t id = fs_find_id(name, (uint16_t)parent_id);
    if (id < 0) return -1;
    return fs_read_file((uint16_t)id, dest, offset, len);
}

static void abi_fs_delete(const char *name, uint32_t parent_id)
{
    int16_t id = fs_find_id(name, (uint16_t)parent_id);
    if (id >= 0) {
        fs_delete((uint16_t)id);
    }
}

/**
 * @brief ABI status LED wrapper.
 * Elevates privileges via ECALL from U-mode to modify hardware RGB state in Machine mode.
 * Explicitly forces a7 = 4 (SYS_SET_LED_COLOR) to avoid compiler register aliasing.
 *
 * @param r Red channel intensity (0-255).
 * @param g Green channel intensity (0-255).
 * @param b Blue channel intensity (0-255).
 */
static void bios_set_led_color(uint8_t r, uint8_t g, uint8_t b)
{
    asm volatile (
        "mv a0, %0\n"
        "mv a1, %1\n"
        "mv a2, %2\n"
        "li a7, 4\n" // SYS_SET_LED_COLOR = 4
        "ecall\n"
        :
        : "r"(r), "r"(g), "r"(b)
        : "a0", "a1", "a2", "a7", "memory"
    );
}

static int32_t bios_net_get_ip(char *buf, uint32_t max_len)
{
    register uint32_t a7 asm("a7") = SYS_NET_GET_IP;
    register uint32_t a0 asm("a0") = (uint32_t)buf;
    register uint32_t a1 asm("a1") = max_len;
    asm volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
    return (int32_t)a0;
}

static int32_t bios_tcp_listen(uint16_t port)
{
    register uint32_t a7 asm("a7") = SYS_TCP_LISTEN;
    register uint32_t a0 asm("a0") = port;
    asm volatile ("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return (int32_t)a0;
}

static int32_t bios_tcp_accept(int32_t server_fd, uint32_t timeout_ms)
{
    register uint32_t a7 asm("a7") = SYS_TCP_ACCEPT;
    register uint32_t a0 asm("a0") = (uint32_t)server_fd;
    register uint32_t a1 asm("a1") = timeout_ms;
    asm volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
    return (int32_t)a0;
}

static int32_t bios_tcp_read(int32_t fd, void *buf, uint32_t len)
{
    register uint32_t a7 asm("a7") = SYS_TCP_READ;
    register uint32_t a0 asm("a0") = (uint32_t)fd;
    register uint32_t a1 asm("a1") = (uint32_t)buf;
    register uint32_t a2 asm("a2") = len;
    asm volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return (int32_t)a0;
}

static int32_t bios_tcp_write(int32_t fd, const void *buf, uint32_t len)
{
    register uint32_t a7 asm("a7") = SYS_TCP_WRITE;
    register uint32_t a0 asm("a0") = (uint32_t)fd;
    register uint32_t a1 asm("a1") = (uint32_t)buf;
    register uint32_t a2 asm("a2") = len;
    asm volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return (int32_t)a0;
}

static void bios_tcp_close(int32_t fd)
{
    register uint32_t a7 asm("a7") = SYS_TCP_CLOSE;
    register uint32_t a0 asm("a0") = (uint32_t)fd;
    asm volatile ("ecall" : "+r"(a0) : "r"(a7) : "memory");
}

static int32_t bios_gpio_set_dir(uint32_t pin, uint32_t is_output)
{
    register uint32_t a7 asm("a7") = SYS_GPIO_SET_DIR;
    register uint32_t a0 asm("a0") = pin;
    register uint32_t a1 asm("a1") = is_output;
    asm volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
    return (int32_t)a0;
}

static int32_t bios_gpio_write(uint32_t pin, uint32_t level)
{
    register uint32_t a7 asm("a7") = SYS_GPIO_WRITE;
    register uint32_t a0 asm("a0") = pin;
    register uint32_t a1 asm("a1") = level;
    asm volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
    return (int32_t)a0;
}

static int32_t bios_gpio_read(uint32_t pin)
{
    register uint32_t a7 asm("a7") = SYS_GPIO_READ;
    register uint32_t a0 asm("a0") = pin;
    asm volatile ("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return (int32_t)a0;
}

static const openc6_abi_t bios_abi = {
    .magic            = OPENC6_ABI_MAGIC,
    .version          = OPENC6_ABI_VERSION,
    .sys_reset        = (abi_sys_reset_fn)bios_sys_reset,
    .set_led_color    = bios_set_led_color,
    .delay_ms         = bios_delay_ms,
    .malloc           = bios_malloc,
    .free             = bios_free,
    .print            = bios_print,
    .get_random       = bios_get_random,
    .sha256           = bios_sha256,
    .math_isqrt       = bios_math_isqrt,
    .math_sin_deg     = bios_math_sin_deg,
    .math_cos_deg     = bios_math_cos_deg,

    /* Network Sockets */
    .wifi_is_connected= bios_wifi_is_connected,
    .net_get_ip       = bios_net_get_ip,
    .tcp_listen       = bios_tcp_listen,
    .tcp_accept       = bios_tcp_accept,
    .tcp_read         = bios_tcp_read,
    .tcp_write        = bios_tcp_write,
    .tcp_close        = bios_tcp_close,

    .get_free_ram     = bios_get_free_ram,
    .get_total_ram    = bios_get_total_ram,
    .get_total_flash  = bios_get_total_flash,
    .fs_write_file    = abi_fs_write_file,
    .fs_read_file     = abi_fs_read_file,
    .fs_delete        = abi_fs_delete,

    /* Hardware GPIO */
    .gpio_set_dir     = bios_gpio_set_dir,
    .gpio_write       = bios_gpio_write,
    .gpio_read        = bios_gpio_read
};

const openc6_abi_t *bios_get_abi(void)
{
    return &bios_abi;
}

/**
 * @brief Low-level POSIX _write syscall hook routing stdout/stderr directly to bare-metal USB FIFO.
 */
int _write(int fd, const char *ptr, int len)
{
    if (fd == 1 || fd == 2) {
        hw_usb_write_bytes((const uint8_t *)ptr, (uint32_t)len);
        return len;
    }
    return -1;
}

void bios_enter_s5_state(void)
{
    ESP_LOGW(TAG, "Entering S5 Soft-Off state...");
    led_mgmt_set_aura_mode(AURA_DISABLED);
    led_mgmt_set_color(0, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(100));

    wifi_mgmt_disconnect();
    esp_wifi_stop();

    /* PIN 3: Virtual ground locked to 0V across deep sleep */
    gpio_reset_pin((gpio_num_t)PIN_BTN_GND);
    gpio_set_direction((gpio_num_t)PIN_BTN_GND, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)PIN_BTN_GND, 0);
    gpio_hold_en((gpio_num_t)PIN_BTN_GND);

    /* PIN 4: Sense input with pullup enabled */
    gpio_reset_pin((gpio_num_t)PIN_BTN_SENSE);
    gpio_set_direction((gpio_num_t)PIN_BTN_SENSE, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)PIN_BTN_SENSE, GPIO_PULLUP_ONLY);

    /* Hardware wakeup trigger via EXT1 on active-low sense line */
    esp_sleep_enable_ext1_wakeup(1ULL << PIN_BTN_SENSE, ESP_EXT1_WAKEUP_ANY_LOW);

    if (is_me_enabled) {
        ESP_LOGI(TAG, "S5: Power Off. Management Engine running in background.");
        ulp_me_hp_is_awake = 0;
        esp_sleep_enable_ulp_wakeup();
    }

    esp_deep_sleep_start();
}

/**
 * @brief Primary OpenC6 BIOS bootstrap and diagnostic entry point.
 * Initializes low-level USB JTAG, decodes reset provenance from the Management Engine,
 * boots the virtual filesystem, provisions the hardware sandbox, and determines
 * execution path (Setup Utility, Remote Web Shell, or Direct Payload Launch).
 */
void bios_core_start(void)
{
    hw_usb_init();
    esp_log_set_vprintf(bios_vprintf);

    esp_reset_reason_t rst_reason = esp_reset_reason();
    bool is_cold_boot = (rst_reason != ESP_RST_DEEPSLEEP);
    uint32_t legacy_hold_ms = 0;

    /* Measure physical power button press duration upon wakeup from S5 deep sleep */
    if (!is_cold_boot && (esp_sleep_get_wakeup_causes() & (1ULL << ESP_SLEEP_WAKEUP_EXT1))) {
        gpio_reset_pin((gpio_num_t)PIN_BTN_GND);
        gpio_set_direction((gpio_num_t)PIN_BTN_GND, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)PIN_BTN_GND, 0);

        gpio_reset_pin((gpio_num_t)PIN_BTN_SENSE);
        gpio_set_direction((gpio_num_t)PIN_BTN_SENSE, GPIO_MODE_INPUT);
        gpio_set_pull_mode((gpio_num_t)PIN_BTN_SENSE, GPIO_PULLUP_ONLY);

        while (gpio_get_level((gpio_num_t)PIN_BTN_SENSE) == 0 && legacy_hold_ms < 3000) {
            vTaskDelay(pdMS_TO_TICKS(10));
            legacy_hold_ms += 10;
        }
    }

    /* Release hold latches and lock PIN 3 as strong 0V GND during runtime */
    gpio_hold_dis((gpio_num_t)PIN_BTN_GND);
    gpio_reset_pin((gpio_num_t)PIN_BTN_GND);
    gpio_set_direction((gpio_num_t)PIN_BTN_GND, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)PIN_BTN_GND, 0);

    /* Enforce internal pullup on sense line */
    gpio_reset_pin((gpio_num_t)PIN_BTN_SENSE);
    gpio_set_direction((gpio_num_t)PIN_BTN_SENSE, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)PIN_BTN_SENSE, GPIO_PULLUP_ONLY);

    uint32_t me_reason = management_engine_get_boot_reason();
    management_engine_clear_boot_reason();

    char boot_info[160];
    snprintf(boot_info, sizeof(boot_info),
             "--- OpenC6 BIOS v2.0-ME Initializing (ME Reason: 0x%08lx, RST Reason: %d) ---\r\n",
             (unsigned long)me_reason, (int)rst_reason);
    hw_usb_print(boot_info);

    nvram_init();
    led_mgmt_init();
    wifi_mgmt_init();
    hal_flash_init();
    fs_init();

    gpio_reset_pin((gpio_num_t)PIN_BTN_BOOT);
    gpio_set_direction((gpio_num_t)PIN_BTN_BOOT, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)PIN_BTN_BOOT, GPIO_PULLUP_ONLY);

    /* Launch Management Engine co-processor on LP-Core to supervise host watchdog and thermals */
    is_me_enabled = true;
    management_engine_init(true, is_cold_boot);
    ulp_me_hp_is_awake = 1;

    bios_update_state_t ota_state;
    nvram_get_bios_update_state(&ota_state);

    if (ota_state == BIOS_UPDATE_PENDING) {
        nvram_set_bios_update_state(BIOS_UPDATE_NONE);
        led_mgmt_set_aura_mode(AURA_DISABLED);
        led_mgmt_set_color(0, 0, 255);

        if (wifi_mgmt_start_sta() == ESP_OK) {
            char pxe_url[PXE_URL_MAX_LEN + 1] = {0};
            nvram_get_pxe_url(pxe_url, sizeof(pxe_url));

            if (pxe_bios_ota_execute(pxe_url)) {
                led_mgmt_set_color(0, 255, 0);
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_restart();
            } else {
                led_mgmt_set_color(255, 0, 0);
                vTaskDelay(pdMS_TO_TICKS(2000));
            }
        }
    }

    gpio_reset_pin((gpio_num_t)PIN_CMOS_GND);
    gpio_set_direction((gpio_num_t)PIN_CMOS_GND, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)PIN_CMOS_GND, 0);

    gpio_reset_pin((gpio_num_t)PIN_CLEAR_NVRAM);
    gpio_set_direction((gpio_num_t)PIN_CLEAR_NVRAM, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)PIN_CLEAR_NVRAM, GPIO_PULLUP_ONLY);

    vTaskDelay(pdMS_TO_TICKS(10));

    /* Hardware CMOS Clear jumper check: revert NVRAM to baseline schema defaults */
    if (gpio_get_level((gpio_num_t)PIN_CLEAR_NVRAM) == 0) {
        led_mgmt_set_aura_mode(AURA_DISABLED);
        led_mgmt_set_color(COLOR_POST_ERROR);
        nvram_load_defaults();
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }
    gpio_set_direction((gpio_num_t)PIN_CMOS_GND, GPIO_MODE_DISABLE);

    power_tweaker_apply_bios_settings();
    led_mgmt_blink_post(COLOR_POST_OK, 3);

    /* Apply configured Aura RGB lighting profile immediately after diagnostic POST */
    led_mgmt_apply_aura_from_nvram();

    esp_ota_img_states_t ota_img_state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &ota_img_state) == ESP_OK) {
        if (ota_img_state == ESP_OTA_IMG_PENDING_VERIFY) {
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }

    bool direct_flash_boot = false;
    bool enter_shell = false;
    bool enter_setup = false;

    /* Interactive boot menu triggered by physical BOOT pin hold */
    if (gpio_get_level((gpio_num_t)PIN_BTN_BOOT) == 0) {
        uint32_t hold_time = 0;
        while (gpio_get_level((gpio_num_t)PIN_BTN_BOOT) == 0 && hold_time < 1000) {
            vTaskDelay(pdMS_TO_TICKS(10));
            hold_time += 10;
        }

        if (hold_time >= 1000) {
            boot_option_t user_choice = boot_manager_interactive_menu();
            if (user_choice == BOOT_OPT_DEFAULT) direct_flash_boot = true;
            else if (user_choice == BOOT_OPT_SHELL) enter_shell = true;
            else if (user_choice == BOOT_OPT_SETUP) enter_setup = true;
            goto execute_boot;
        }
    }

    /* Evaluate hardware reset provenance */
    if (me_reason == ME_BOOT_REASON_THERMAL) {
        hw_usb_print("\r\n[THERMAL_CRITICAL] Emergency shutdown! Core temperature exceeded limit (75 C).\r\n");
        hw_usb_print("[THERMAL_CRITICAL] Entering S5 Soft-Off state to prevent silicon damage.\r\n");
        vTaskDelay(pdMS_TO_TICKS(100));
        bios_enter_s5_state();
    } else if (me_reason == ME_BOOT_REASON_FORCE_RESET) {
        hw_usb_print("\r\n[PWR_MGMT] Hard shutdown initiated by physical power button hold (3s).\r\n");
        vTaskDelay(pdMS_TO_TICKS(100));
        bios_enter_s5_state();
    } else if (me_reason == ME_BOOT_REASON_SETUP || legacy_hold_ms >= 3000) {
        hw_usb_print("\r\n[BOOT_TRIGGER] Entering BIOS Setup Utility (Triggered by Power Button hold).\r\n");
        enter_setup = true;
    } else if (me_reason == ME_BOOT_REASON_WDT) {
        hw_usb_print("\r\n[WATCHDOG_RECOVERY] Kernel lockup detected! System was reset by LP-Core Management Engine (15s timeout).\r\n");
        enter_shell = true;
    } else if (rst_reason == ESP_RST_PANIC) {
        hw_usb_print("\r\n[KERNEL_RECOVERY] System recovered from CPU Exception / Panic. Launching Shell.\r\n");
        enter_shell = true;
    } else if (me_reason == ME_BOOT_REASON_NORMAL || legacy_hold_ms > 0 || rst_reason == ESP_RST_SW || rst_reason == ESP_RST_DEEPSLEEP) {
        direct_flash_boot = true;
    } else if (is_cold_boot) {
        dc_loss_action_t dc_action;
        nvram_get_dc_loss_action(&dc_action);
        if (dc_action == DC_LOSS_POWER_OFF) {
            hw_usb_print("\r\n[PWR_MGMT] Cold power-on detected with DC_LOSS = POWER_OFF. Entering S5.\r\n");
            vTaskDelay(pdMS_TO_TICKS(100));
            bios_enter_s5_state();
        } else {
            direct_flash_boot = true;
        }
    }

    execute_boot:
    if (enter_setup) {
        enter_setup = false;
        aura_mode_t aura;
        nvram_get_aura_mode(&aura);
        if (aura == AURA_RAINBOW) {
            led_mgmt_set_aura_mode(AURA_RAINBOW);
        } else {
            led_mgmt_set_color(COLOR_BIOS_SETUP);
        }

        wifi_mgmt_start_ap(BIOS_AP_SSID, "12345678");
        web_ui_start();

        while (1) {
            management_engine_pet_watchdog();
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    /* Check whether c6wsh remote web shell is enabled in NVRAM */
    c6wsh_state_t wsh_state = C6WSH_DISABLED;
    nvram_get_c6wsh_state(&wsh_state);

    if (wsh_state == C6WSH_ENABLED) {
        hw_usb_print("\r\n[C6WSH] Web Shell is ENABLED in NVRAM. Establishing Wi-Fi...\r\n");

        if (wifi_mgmt_start_sta() == ESP_OK) {
            /* Attach telemetry tap so all console output routes into c6wsh streaming pipe */
            hw_usb_set_tap(c6wsh_write_output);
            c6wsh_start(bios_get_abi());

            char ip_buf[32] = "0.0.0.0";
            esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (sta_netif) {
                esp_netif_ip_info_t ip_info;
                if (esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK) {
                    snprintf(ip_buf, sizeof(ip_buf), IPSTR, IP2STR(&ip_info.ip));
                }
            }

            char notify[96];
            snprintf(notify, sizeof(notify), "  c6wsh READY: http://%s/\r\n", ip_buf);

            hw_usb_print("\r\n==================================================\r\n");
            hw_usb_print(notify);
            hw_usb_print("==================================================\r\n");

            extern void c6wsh_execute_command(const char *cmd_line);
            extern bool c6wsh_get_pending_command(char *dest, size_t max_len);

            char web_cmd[128];
            char usb_cmd[128];
            int usb_idx = 0;

            /* Service loop: process web shell and local USB terminal multiplexing concurrently */
            while (1) {
                management_engine_pet_watchdog();

                /* 1. Dequeue and execute remote commands from Web Shell */
                if (c6wsh_get_pending_command(web_cmd, sizeof(web_cmd))) {
                    c6wsh_execute_command(web_cmd);
                }

                /* 2. Process keystrokes typed on physical USB Type-C console */
                while (hw_usb_has_data()) {
                    char c = hw_usb_getc();
                    if (c == '\r' || c == '\n') {
                        if (usb_idx > 0) {
                            usb_cmd[usb_idx] = '\0';
                            hw_usb_print("\r\n");
                            c6wsh_execute_command(usb_cmd);
                            usb_idx = 0;
                        }
                    } else if (c == '\b' || c == 0x7F) {
                        if (usb_idx > 0) {
                            usb_idx--;
                            usb_cmd[usb_idx] = '\0';
                            hw_usb_print("\b \b");
                        }
                    } else if (usb_idx < (int)sizeof(usb_cmd) - 1) {
                        usb_cmd[usb_idx++] = c;
                        hw_usb_putc((uint8_t)c);
                        hw_usb_flush();
                    }
                }

                vTaskDelay(pdMS_TO_TICKS(20));
            }
        } else {
            hw_usb_print("[C6WSH] Warning: Wi-Fi STA failed. Falling back to local USB shell.\r\n");
            enter_shell = true;
        }
    }

    if (enter_shell) {
        enter_shell = false;
        boot_manager_shell(bios_get_abi());
        goto execute_boot;
    }

    /* Standard payload launch path: executed exclusively in U-Mode under PMP hardware isolation */
    if (direct_flash_boot) {
        direct_flash_boot = false;
        const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0xff, "openc6_fs");
        if (part) {
            uint16_t boot_dir_id = 0;
            int16_t dir_id = fs_find_id("downloaded", 0);
            if (dir_id >= 0 && fs_get_type(dir_id) == TYPE_DIR) {
                boot_dir_id = (uint16_t)dir_id;
            }

            int16_t file_id = fs_find_id("payload.bin", boot_dir_id);
            if (file_id >= 0 && fs_get_type(file_id) == TYPE_FILE) {
                int32_t file_size = fs_get_size(file_id);
                if (file_size > 0) {
                    sandbox_init_dynamic(bios_get_abi(), SANDBOX_DEFAULT_KERNEL_RESERVE);
                    size_t arena_sz = sandbox_get_arena_size();

                    if ((size_t)file_size <= arena_sz) {
                        led_mgmt_set_aura_mode(AURA_DISABLED);
                        led_mgmt_set_color(0, 0, 0);

                        uint8_t *arena = sandbox_get_arena();
                        if (fs_read_file(file_id, arena, 0, (uint32_t)file_size) == file_size) {
                            ESP_LOGI(TAG, "Launching payload via U-mode Sandbox (%ld bytes)...", (long)file_size);
                            sandbox_load_payload(arena, (size_t)file_size);
                            sandbox_status_t status = sandbox_run();
                            ESP_LOGW(TAG, "Sandbox session ended: status=%d", status);
                            sandbox_deinit();

                            led_mgmt_apply_aura_from_nvram();
                            enter_shell = true;
                            goto execute_boot;
                        }
                        sandbox_deinit();
                        led_mgmt_apply_aura_from_nvram();
                    } else {
                        ESP_LOGE(TAG, "Payload size (%ld B) exceeds dynamic Sandbox arena (%zu B)",
                                 (long)file_size, arena_sz);
                    }
                }
            }
        }

        ESP_LOGW(TAG, "Default payload unavailable. Opening Shell...");
        enter_shell = true;
        goto execute_boot;
    }

    bios_enter_s5_state();
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
