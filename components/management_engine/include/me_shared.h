#pragma once
#include <stdint.h>

// --- System Boot Reasons ---
#define ME_BOOT_REASON_NONE         0
#define ME_BOOT_REASON_NORMAL       1
#define ME_BOOT_REASON_SETUP        2  // Power button held for 3 seconds
#define ME_BOOT_REASON_FORCE_RESET  3  // POWER button held for 5s (or BOOT held for 200ms)
#define ME_BOOT_REASON_WDT          4  // LP WDT: High-Performance (HP) core locked up/deadlocked
#define ME_BOOT_REASON_THERMAL      5  // Emergency Thermal Reset: Junction temperature exceeded limit (>90°C)

// --- Status Flags (Shared LP-RAM) ---
// Note: The ULP compiler automatically exports these variables with a "ulp_" prefix.

extern uint32_t ulp_me_hp_is_awake;
extern uint32_t ulp_me_boot_reason;
extern uint32_t ulp_me_wdt_counter;

// --- SchedUtil & Thermal Protection Metrics ---

/**
 * HP Core writes the CPU load percentage (0-100) here every 100ms.
 * LP Core reads this value to scale the CPU frequency dynamically.
 */
extern uint32_t ulp_me_cpu_load;

/**
 * HP Core writes the frequency upper bound from NVRAM (80, 120, or 160 MHz).
 * LP Core restricts target_freq to not exceed this boundary.
 */
extern uint32_t ulp_me_max_freq;

/**
 * LP Core writes the calculated frequency target here (80, 120, 160 MHz).
 * HP Core reads this value and applies the frequency change in hardware.
 */
extern uint32_t ulp_me_target_freq;

/**
 * LP Core writes the raw junction temperature (in Celsius) here.
 * BIOS reads this metric for system monitoring and displays it on the Web UI.
 */
extern uint32_t ulp_me_temperature;

extern uint32_t ulp_me_throttle_temp;  // HP writes, LP reads to initiate throttling
extern uint32_t ulp_me_emergency_temp; // HP writes, LP reads to trigger emergency shutdown
