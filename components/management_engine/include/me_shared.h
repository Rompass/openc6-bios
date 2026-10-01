#pragma once
#include <stdint.h>

/* System Boot Reasons evaluated by Management Engine */
#define ME_BOOT_REASON_NONE         0
#define ME_BOOT_REASON_NORMAL       1
#define ME_BOOT_REASON_SETUP        2  /* Power button held for >= 3 seconds */
#define ME_BOOT_REASON_FORCE_RESET  3  /* Power button held for >= 5s (hardware override) */
#define ME_BOOT_REASON_WDT          4  /* LP WDT: High-Performance (HP) core locked up */
#define ME_BOOT_REASON_THERMAL      5  /* Emergency Thermal Reset: Junction temp exceeded safety ceiling */

/* Status Flags (Shared LP-RAM)
 * The ESP32-C6 ULP toolchain automatically prefixes exported symbols with "ulp_" */

extern uint32_t ulp_me_hp_is_awake;
extern uint32_t ulp_me_boot_reason;
extern uint32_t ulp_me_wdt_counter;

/* ─── SchedUtil & Autonomous Governor Metrics ────────────────────────────── */

/**
 * @brief High-Performance (HP) core busy tick accumulator.
 * Incremented during FreeRTOS SysTick whenever active work (U-mode payload or kernel) executes.
 * Read by LP-Core to calculate precise CPU utilization independent of privilege modes.
 */
extern uint32_t ulp_me_hp_busy_ticks;

/**
 * @brief High-Performance (HP) core idle tick accumulator.
 * Incremented during FreeRTOS SysTick exclusively when the Idle task is executing.
 */
extern uint32_t ulp_me_hp_idle_ticks;

/**
 * @brief Calculated CPU load percentage (0-100) determined autonomously by LP-Core.
 * Exported for telemetry, shell diagnostics, and Web UI metrics.
 */
extern uint32_t ulp_me_cpu_load;

/**
 * @brief Frequency upper bound configured in CMOS NVRAM (80, 120, or 160 MHz).
 * LP-Core enforces this hardware clamp.
 */
extern uint32_t ulp_me_max_freq;

/**
 * @brief Active hardware frequency applied directly by LP-Core via PCR registers (80, 120, 160 MHz).
 * Read by HP-Core to keep ROM microsecond delay timers synchronized.
 */
extern uint32_t ulp_me_target_freq;

/**
 * @brief Raw junction temperature in Celsius.
 * Read by BIOS diagnostics and Web UI.
 */
extern uint32_t ulp_me_temperature;

extern uint32_t ulp_me_throttle_temp;  /* Dynamic throttle threshold from NVRAM */
extern uint32_t ulp_me_emergency_temp; /* Dynamic emergency shutdown threshold from NVRAM */
