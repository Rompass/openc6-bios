#pragma once

#include <stdint.h>

// Integrity marker "C6BI" (C6 BIOS Interface) to validate the calling context
#define OPENC6_ABI_MAGIC   0x43364249
#define OPENC6_ABI_VERSION 1

/**
 * @brief Application Binary Interface (ABI) definition passed to independent payloads.
 * Serves as a secure jump-table, decoupling payload compilation from the static address mapping of the host.
 */
typedef struct {
    uint32_t magic;    // Must evaluate to OPENC6_ABI_MAGIC
    uint32_t version;  // Must evaluate to OPENC6_ABI_VERSION

    // ─── SYSTEM API WRAPPERS ─────────────────────────────────────────────────

    // Triggers a hardware reset of the core, returning context to the host BIOS
    void (*sys_reset)(void) __attribute__((noreturn));

    // Manipulates the color of the onboard system RGB status LED
    void (*set_led_color)(uint8_t r, uint8_t g, uint8_t b);

    // Yields executing thread time to the FreeRTOS scheduler to prevent WDT trigger
    void (*delay_ms)(uint32_t ms);

    // Allocates dynamic memory blocks directly from the host system heap
    void* (*malloc)(uint32_t size);

    // Frees memory blocks previously obtained from the host system heap
    void (*free)(void* ptr);

    // Prints diagnostic and telemetry text directly to the active BIOS console
    void (*print)(const char *str);

    // Generates a cryptographically secure hardware-generated random number (TRNG)
    uint32_t (*get_random)(void);

    // Performs high-speed, hardware-accelerated SHA-256 calculation on ESP32-C6 silicon
    void (*sha256)(const uint8_t *input, uint32_t len, uint8_t *output);

    // ─── SYSTEM MATHEMATICAL FUNCTIONS (FPU-LESS SYSTEMS OPTIMIZATION) ───────

    // Calculates high-speed integer square root
    uint32_t (*math_isqrt)(uint32_t x);

    // Fixed-point trigonometric sine calculation. Input: deg (0-360), output scaled by 10000
    // Example: sin(90) returns 10000, sin(30) returns 5000 (0.5000)
    int32_t (*math_sin_deg)(int32_t angle_deg);
    int32_t (*math_cos_deg)(int32_t angle_deg);

    // ─── WIRELESS NETWORK SYSTEM INTERFACES ──────────────────────────────────

    // Authenticates and connects to a localized Wi-Fi Access Point. Returns 0 on success.
    int32_t (*wifi_connect)(const char* ssid, const char* pass);

    // Spawns a dedicated local wireless configuration Access Point (AP). Returns 0 on success.
    int32_t (*wifi_start_ap)(const char* ssid, const char* pass);

    // Verifies the connection state of the internal Wi-Fi station (1: Connected, 0: Disconnected)
    int32_t (*wifi_is_connected)(void);

    // ─── TELEMETRY AND HEAP MONITORING INTERFACES ────────────────────────────

    // Retrieves current total available free Internal DRAM heap bytes
    uint32_t (*get_free_ram)(void);

    // Retrieves the absolute size boundary of the host Internal DRAM heap space
    uint32_t (*get_total_ram)(void);

    // Retrieves the absolute size boundary of the integrated system Flash media
    uint32_t (*get_total_flash)(void);

} openc6_abi_t;
