#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "rom/ets_sys.h"
#include "boot_manager.h"
#include "me_shared.h"
#include "led_mgmt.h"
#include "openc6_abi.h"
#include "esp_random.h"
#include "psa/crypto.h"
#include <math.h>
#include "esp_flash.h"
#include "nvram.h"
#include "wifi_mgmt.h"

#ifndef MALLOC_CAP_EXEC
#error "RISC-V Memory Protection is ENABLED! You must disable CONFIG_ESP_SYSTEM_MEMPROT_FEATURE in menuconfig to allow RAM execution."
#endif

static const char *TAG = "BOOT_MGR";

#define PIN_BTN_BOOT 9

// Synchronization Protocol Commands
#define CMD_ACK     0x06
#define CMD_EOT     0x04
#define CMD_NAK     0x15

// ─────────────────────────────────────────────────────────────────────────────
#define USE_EXTERNAL_CP2102 1
#define EXTERNAL_UART_TX_PIN 18  // <--- RE-ROUTED PHYSICAL TX PIN
#define EXTERNAL_UART_RX_PIN 19  // <--- RE-ROUTED PHYSICAL RX PIN
// ─────────────────────────────────────────────────────────────────────────────

// 1. Create small wrappers for ABI type compatibility
static void bios_delay_ms(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static void* bios_malloc(uint32_t size) {
    return malloc((size_t)size);
}

// ─── CONSOLE PRINT WRAPPER (PRINT) ──────────────────────────────────────────────

// Output text directly to the active BIOS console for debugging inside payloads
static void bios_print(const char *str) {
    printf("%s", str);
}

static void bios_sha256(const uint8_t *input, uint32_t len, uint8_t *output) {
    // Initialize crypto subsystem (if already initialized, it will return success safely)
    psa_crypto_init();

    size_t hash_len;
    // Compute hash using the universal hardware-accelerated API
    psa_hash_compute(PSA_ALG_SHA_256,
                     input, len,
                     output, 32,
                     &hash_len);
}

// ─── WI-FI ABI WRAPPERS ──────────────────────────────────────────────────

static int32_t bios_wifi_connect(const char* ssid, const char* pass) {
    // Override Wi-Fi credentials in NVRAM and initiate connection
    nvram_set_wifi_sta_config(ssid, pass);
    return (wifi_mgmt_start_sta() == ESP_OK) ? 0 : -1;
}

static int32_t bios_wifi_start_ap(const char* ssid, const char* pass) {
    return (wifi_mgmt_start_ap(ssid, pass) == ESP_OK) ? 0 : -1;
}

static int32_t bios_wifi_is_connected(void) {
    return wifi_mgmt_is_connected() ? 1 : 0;
}

// ─── SYSTEM AND MEMORY METRICS ABI WRAPPERS ────────────────────────────────────

static uint32_t bios_get_free_ram(void) {
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static uint32_t bios_get_total_ram(void) {
    return heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static uint32_t bios_get_total_flash(void) {
    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        return flash_size;
    }
    return 0;
}

// ─── MATHEMATICAL COMPATIBILITY ABI WRAPPERS (FPU-LESS PAYLOAD SUPPORT) ────────────────────────

// Fast integer square root (avoiding heavy floating-point calculation)
static uint32_t bios_math_isqrt(uint32_t x) {
    uint32_t res = 0;
    uint32_t bit = 1UL << 30;
    while (bit > x) bit >>= 2;
    while (bit != 0) {
        if (x >= res + bit) {
            x -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return res;
}

// Calculate sine inside BIOS and return fixed-point result scaled by 10000
static int32_t bios_math_sin_deg(int32_t deg) {
    double rad = deg * (3.14159265359 / 180.0);
    return (int32_t)(sin(rad) * 10000.0);
}

// Same fixed-point scaling for cosine calculation inside BIOS
static int32_t bios_math_cos_deg(int32_t deg) {
    double rad = deg * (3.14159265359 / 180.0);
    return (int32_t)(cos(rad) * 10000.0);
}

static const openc6_abi_t bios_abi = {
    .magic = OPENC6_ABI_MAGIC,
    .version = OPENC6_ABI_VERSION,
    .sys_reset = esp_restart,
    .set_led_color = led_mgmt_set_color,
    .delay_ms = bios_delay_ms,
    .malloc = bios_malloc,
    .free = free,
    .print = bios_print,
    .get_random = esp_random,
    .sha256 = bios_sha256,

    // Integer-only math extensions
    .math_isqrt = bios_math_isqrt,
    .math_sin_deg = bios_math_sin_deg,
    .math_cos_deg = bios_math_cos_deg,

    // Wi-Fi subsystem APIs
    .wifi_connect = bios_wifi_connect,
    .wifi_start_ap = bios_wifi_start_ap,
    .wifi_is_connected = bios_wifi_is_connected,

    // System telemetry and RAM sizing
    .get_free_ram = bios_get_free_ram,
    .get_total_ram = bios_get_total_ram,
    .get_total_flash = bios_get_total_flash
};

// ─── PORT DEFINITIONS & MACROS ───────────────────────────────────────────────
#if USE_EXTERNAL_CP2102
#define PORT_NUM UART_NUM_1
#define PORT_READ(buf, len)         uart_read_bytes(PORT_NUM, buf, len, 0)
#define PORT_WRITE(buf, len)        uart_write_bytes(PORT_NUM, buf, len)
#define PORT_FLUSH()                uart_flush_input(PORT_NUM)
#else
#define PORT_READ(buf, len)         usb_serial_jtag_read_bytes(buf, len, 0)
#define PORT_WRITE(buf, len)        usb_serial_jtag_write_bytes(buf, len, 0)
#define PORT_FLUSH()
#endif

static void bios_serial_init(void) {
    #if USE_EXTERNAL_CP2102
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(PORT_NUM, &uart_config);
    uart_set_pin(PORT_NUM, EXTERNAL_UART_TX_PIN, EXTERNAL_UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(PORT_NUM, 1024, 1024, 0, NULL, 0);
    ESP_LOGI(TAG, "Standard UART1 Driver activated. TX=%d, RX=%d", EXTERNAL_UART_TX_PIN, EXTERNAL_UART_RX_PIN);
    #else
    usb_serial_jtag_driver_config_t usj_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb_serial_jtag_driver_install(&usj_cfg);
    ESP_LOGI(TAG, "Standard USB_SERIAL_JTAG Driver activated.");
    #endif
}

static void bios_serial_deinit(void) {
    #if USE_EXTERNAL_CP2102
    uart_driver_delete(PORT_NUM);
    #endif
}

static void blink_menu(boot_option_t opt) {
    ESP_LOGI(TAG, "-> Current Option: [%d] %s", opt,
             opt == BOOT_OPT_NETWORK ? "Network Boot (PXE)" :
             opt == BOOT_OPT_SERIAL_RAM  ? "Serial Boot (RAM)" :
             opt == BOOT_OPT_SERIAL_FLASH ? "Serial Boot (FLASH)" :
             opt == BOOT_OPT_DEFAULT ? "Default OS (Flash)" : "BIOS Setup");
    led_mgmt_set_aura_mode(AURA_DISABLED);
    led_mgmt_blink_post(0, 255, 255, opt + 1);
}

boot_option_t boot_manager_interactive_menu(void) {
    ESP_LOGW(TAG, "=== INTERACTIVE BOOT MENU ===");
    ESP_LOGI(TAG, "Short press (<500ms): Next Option");
    ESP_LOGI(TAG, "Long press  (>1sec) : Select Option");

    gpio_reset_pin((gpio_num_t)PIN_BTN_BOOT);
    gpio_set_direction((gpio_num_t)PIN_BTN_BOOT, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)PIN_BTN_BOOT, GPIO_PULLUP_ONLY);

    boot_option_t current_opt = BOOT_OPT_NETWORK;
    blink_menu(current_opt);

    while (1) {
        if (gpio_get_level((gpio_num_t)PIN_BTN_BOOT) == 0) {
            uint32_t press_duration_ms = 0;
            while (gpio_get_level((gpio_num_t)PIN_BTN_BOOT) == 0) {
                vTaskDelay(pdMS_TO_TICKS(10));
                press_duration_ms += 10;
            }
            if (press_duration_ms >= 1000) {
                ESP_LOGW(TAG, "Option Selected: %d", current_opt);
                led_mgmt_set_color(0, 255, 0);
                vTaskDelay(pdMS_TO_TICKS(500));
                return current_opt;
            } else if (press_duration_ms >= 50) {
                current_opt = (boot_option_t)((current_opt + 1) % BOOT_OPT_MAX);
                blink_menu(current_opt);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ─── PAYLOAD RECEIVER AND BOOTSTRAPPER ───────────────────────────────────────

bool boot_manager_serial_listen(int timeout_sec, payload_target_t target) {
    ESP_LOGW(TAG, "Entering Serial Boot Mode. Target: %s", target == PAYLOAD_TARGET_RAM ? "RAM" : "FLASH");

    bios_serial_init();
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_log_level_set("*", ESP_LOG_NONE);
    PORT_FLUSH();

    led_mgmt_set_color(255, 0, 255); // Magenta status illumination

    uint8_t size_buf[4] = {0};
    uint32_t file_size = 0;
    uint64_t start_time = esp_timer_get_time();
    uint64_t last_sync_time = 0;
    uint64_t timeout_us = (uint64_t)timeout_sec * 1000000ULL;
    const char *fail_reason = "Unknown Error";

    int state = 0; // State-machine tracker (0: Wait 5A, 1: Wait A5, 2-5: Read size)

    while ((esp_timer_get_time() - start_time) < timeout_us) {
        uint64_t now = esp_timer_get_time();

        if (now - last_sync_time > 500000ULL) {
            const char* sync_msg = "##OPENC6_SYNC##";
            PORT_WRITE(sync_msg, strlen(sync_msg));
            last_sync_time = now;
        }

        uint8_t rx_b;
        while (PORT_READ(&rx_b, 1) > 0) {
            if (state == 0) {
                if (rx_b == 0x5A) state = 1;
            } else if (state == 1) {
                if (rx_b == 0xA5) state = 2;
                else state = (rx_b == 0x5A) ? 1 : 0;
            } else if (state >= 2 && state <= 5) {
                size_buf[state - 2] = rx_b;
                state++;
                if (state == 6) break;
            }
        }

        if (state == 6) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (state != 6) {
        fail_reason = "Timeout waiting for PC to send Size Preamble (5A A5)";
        goto error_exit;
    }

    file_size = (uint32_t)size_buf[0] | ((uint32_t)size_buf[1] << 8) | ((uint32_t)size_buf[2] << 16) | ((uint32_t)size_buf[3] << 24);

    if (file_size == 0 || file_size > (target == PAYLOAD_TARGET_RAM ? 500000 : 5000000)) {
        uint8_t nak = CMD_NAK; PORT_WRITE(&nak, 1);
        fail_reason = "Invalid file size bounds configured";
        goto error_exit;
    }

    // ─── MEMORY PROVISIONING (RAM OR FLASH TARGET) ───────────────────────────
    void *payload_ram_ptr = NULL;
    const esp_partition_t *part = NULL;

    if (target == PAYLOAD_TARGET_RAM) {
        uint32_t alloc_size = (file_size + 3) & ~3;
        payload_ram_ptr = heap_caps_malloc(alloc_size, MALLOC_CAP_EXEC | MALLOC_CAP_INTERNAL);
        if (!payload_ram_ptr) {
            uint8_t nak = CMD_NAK; PORT_WRITE(&nak, 1);
            fail_reason = "Internal RAM allocation failed";
            goto error_exit;
        }
    } else {
        part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0xff, "network_buf");
        if (!part || file_size > part->size) {
            uint8_t nak = CMD_NAK; PORT_WRITE(&nak, 1);
            fail_reason = "Flash partition 'network_buf' not found or size bounds exceeded";
            goto error_exit;
        }

        // Erase target sectors in Flash (aligned to 4KB boundary offsets)
        uint32_t erase_size = (file_size + 4095) & ~4095;
        esp_partition_erase_range(part, 0, erase_size);
    }

    // Handshake back: "Host system initialized and ready to stream data."
    uint8_t ack = CMD_ACK;
    PORT_WRITE(&ack, 1);

    uint8_t chunk_rx_buf[64];
    int total_received = 0;
    int chunk_accum = 0;
    uint64_t last_data_time = esp_timer_get_time();

    // ─── DATA RECEIVE LOOP ───────────────────────────────────────────────────
    while (total_received < file_size) {
        int to_read = file_size - total_received;
        if (to_read > 64) to_read = 64; // Read in max 64-byte chunks

        int rx_len = PORT_READ(chunk_rx_buf, to_read);

        if (rx_len > 0) {
            // Commit buffered chunks to target media space
            if (target == PAYLOAD_TARGET_RAM) {
                memcpy((uint8_t*)payload_ram_ptr + total_received, chunk_rx_buf, rx_len);
            } else {
                esp_partition_write(part, total_received, chunk_rx_buf, rx_len);
            }

            total_received += rx_len;
            chunk_accum += rx_len;
            last_data_time = esp_timer_get_time();

            // Intermittent ACK response every 64 accumulated bytes
            while (chunk_accum >= 64 || total_received == file_size) {
                PORT_WRITE(&ack, 1);
                chunk_accum -= 64;
                if (total_received == file_size && chunk_accum <= 0) break;
            }
        }

        if ((esp_timer_get_time() - last_data_time) > 5000000ULL) {
            fail_reason = "Timeout during data chunk stream transfer";
            goto error_exit;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }

    // Await EOT (End of Transmission) frame
    uint64_t eot_timeout = esp_timer_get_time();
    while ((esp_timer_get_time() - eot_timeout) < 1000000ULL) {
        uint8_t eot_buf;
        if (PORT_READ(&eot_buf, 1) > 0) {
            if (eot_buf == CMD_EOT) break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    // ─── BOOT HANDOVER & LAUNCH ──────────────────────────────────────────────
    esp_log_level_set("*", ESP_LOG_INFO);
    ESP_LOGI(TAG, "Payload Flashed (%d bytes). Target: %s", total_received, target == PAYLOAD_TARGET_RAM ? "RAM" : "FLASH");
    led_mgmt_set_color(0, 255, 0);
    vTaskDelay(pdMS_TO_TICKS(100));

    bios_serial_deinit();

    // Declarative typedef wrapper for Payload execution mapping
    typedef void (*payload_entry_t)(const openc6_abi_t *) __attribute__((noreturn));

    payload_entry_t launch_payload = NULL;
    esp_partition_mmap_handle_t mmap_handle;

    if (target == PAYLOAD_TARGET_RAM) {
        launch_payload = (payload_entry_t)payload_ram_ptr;
    } else {
        const void *mapped_ptr = NULL;
        // Map Flash block into virtual instruction cache via MMU (XIP)
        esp_partition_mmap(part, 0, file_size, ESP_PARTITION_MMAP_INST, &mapped_ptr, &mmap_handle);
        launch_payload = (payload_entry_t)mapped_ptr;
    }

    asm volatile ("fence.i"); // Synchronize instruction cache pipeline
    launch_payload(&bios_abi); // Yield control to payload with active ABI pointer!

    while (1);

    error_exit:
    esp_log_level_set("*", ESP_LOG_INFO);
    ESP_LOGE(TAG, "CRASH: %s", fail_reason);
    bios_serial_deinit();
    return false;
}
