#pragma once

#include <stdint.h>

#define BIOS_NVS_NAMESPACE "bios_cfg"

/* Basic Configuration Settings */
typedef enum {
    DC_LOSS_POWER_OFF = 0,
    DC_LOSS_POWER_ON  = 1
} dc_loss_action_t;

typedef enum {
    POST_LED_DISABLED = 0,
    POST_LED_ENABLED  = 1
} post_led_mode_t;

/**
 * @brief Aura Sync RGB lighting profiles.
 * Explicitly mirrors the HTML select indices in the BIOS Web Setup UI:
 * 0 = Static Blue, 1 = Rainbow Flow, 2 = Completely Disabled.
 */
typedef enum {
    AURA_STATIC   = 0,
    AURA_RAINBOW  = 1,
    AURA_DISABLED = 2
} aura_mode_t;

/* AI Tweaker: CPU & Power Tuning Configurations */
typedef enum {
    CPU_FREQ_80MHZ  = 80,
    CPU_FREQ_120MHZ = 120,
    CPU_FREQ_160MHZ = 160,
} cpu_freq_t;

typedef enum {
    GOV_PERFORMANCE = 0,
    GOV_DYNAMIC     = 1,
} cpu_governor_t;

typedef enum {
    BOD_STRICT   = 0,
    BOD_RELAXED  = 1,
    BOD_DISABLED = 2,
} bod_level_t;

/**
 * @brief ZC6 Hardware Compression and ZSWAP Engine modes.
 * 0 = Disabled (Raw .bin execution, no RAM compression)
 * 1 = Flash Only (Transparently decompress .zc6 binaries from filesystem/PXE)
 * 2 = RAM Only (ZSWAP: Compress suspended arena memory pages to free DRAM)
 * 3 = Both Active (Flash FS .zc6 decompression + RAM ZSWAP)
 */
typedef enum {
    ZC6_SWAP_DISABLED = 0,
    ZC6_SWAP_FLASH    = 1,
    ZC6_SWAP_RAM      = 2,
    ZC6_SWAP_BOTH     = 3
} zc6_swap_mode_t;

#define VAL_ZC6_SWAP_DEFAULT ZC6_SWAP_BOTH

typedef enum {
    BIOS_UPDATE_NONE    = 0,
    BIOS_UPDATE_PENDING = 1
} bios_update_state_t;

/**
 * @brief Remote Web Shell (c6wsh) activation state in CMOS NVRAM.
 * When disabled (default), Wi-Fi MAC and baseband remain powered off during POST
 * to conserve ~90 KB internal SRAM for U-mode sandbox payloads and guarantee instant boot.
 * When enabled, BIOS automatically connects to the stored AP and launches the c6wsh daemon.
 */
typedef enum {
    C6WSH_DISABLED = 0,
    C6WSH_ENABLED  = 1
} c6wsh_state_t;

/* Network Interface Configuration Limits */
#define WIFI_SSID_MAX_LEN     32
#define WIFI_PASS_MAX_LEN     64
#define WIFI_COUNTRY_MAX_LEN  3    /* 2-letter ISO 3166-1 alpha-2 string + null terminator */
#define PXE_URL_MAX_LEN       128

/* Regulatory Domain Defaults */
#define VAL_WIFI_COUNTRY_DEFAULT "01" /* World Safe Mode (Channels 1-13, auto-policy) */

/* AI Tweaker: Extended Safety Tuning (Thermal Limits) */
#define VAL_TEMP_THROTTLE_DEFAULT  55
#define VAL_TEMP_EMERGENCY_DEFAULT 75

/* Default PXE Configurations */
#define VAL_PXE_URL_DEFAULT "http://192.168.1.1:8080/payload.bin"
