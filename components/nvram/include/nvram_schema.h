#pragma once
#include <stdint.h>

#define BIOS_NVS_NAMESPACE "bios_cfg"

// --- Basic Configuration Settings ---
typedef enum { DC_LOSS_POWER_OFF = 0, DC_LOSS_POWER_ON = 1 } dc_loss_action_t;
typedef enum { POST_LED_DISABLED = 0, POST_LED_ENABLED = 1 } post_led_mode_t;
typedef enum { AURA_DISABLED = 0, AURA_RAINBOW = 1 } aura_mode_t;

// --- AI Tweaker: CPU & Power Tuning Configurations ---

// Core clock frequency boundary configuration (CPU Freq)
typedef enum {
    CPU_FREQ_80MHZ  = 80,  // Power Saver / Base clock frequency
    CPU_FREQ_160MHZ = 160, // Turbo / Overclock frequency
} cpu_freq_t;

// CPU Power Governor (Similar to Intel SpeedStep / AMD Cool'n'Quiet)
typedef enum {
    GOV_PERFORMANCE = 0, // Maintain target frequency and maximum VDD continuously
    GOV_DYNAMIC     = 1, // Dynamically scale down frequency and voltage during idle (DVS)
} cpu_governor_t;

// Load-Line / Anti-Surge (Brownout Protection Level)
typedef enum {
    BOD_STRICT   = 0, // Maximum stability: Reset triggered when voltage drops below 2.8V
    BOD_RELAXED  = 1, // Relaxed tolerance: Reset triggered when voltage drops below 2.5V
    BOD_DISABLED = 2, // Extreme mode: Brownout detector disabled (at your own risk)
} bod_level_t;

// --- Management Engine Configuration ---
typedef enum { ME_DISABLED = 0, ME_ENABLED = 1 } me_state_t;

typedef enum { BIOS_UPDATE_NONE = 0, BIOS_UPDATE_PENDING = 1 } bios_update_state_t;

// --- Network Interface Configuration ---
#define WIFI_SSID_MAX_LEN 32
#define WIFI_PASS_MAX_LEN 64
#define PXE_URL_MAX_LEN   128  // <--- Maximum allowed PXE URL string length

// --- AI Tweaker: Extended Safety Tuning (Thermal Limits) ---
// Temperature thresholds for throttling and emergency shutdown (in Celsius)
#define VAL_TEMP_THROTTLE_DEFAULT  55
#define VAL_TEMP_EMERGENCY_DEFAULT 75

// --- Default PXE Configurations ---
#define VAL_PXE_URL_DEFAULT "http://192.168.1.1:8080/payload.bin" // <--- Default fallback address for PXE update server

