#include <stdint.h>
#include <stdbool.h>
#include "ulp_lp_core_utils.h"
#include "ulp_lp_core_gpio.h"
#include "me_shared.h"

// ─── SYSTEM GPIO MAPPING ──────────────────────────────────────────────────────
#define PIN_BTN_GND   3
#define PIN_BTN_SENSE 4

// ─── ESP32-C6 LP COPROCESSOR WATCHDOG (LP_WDT/RWDT) REGISTERS ─────────────────
#define LP_WDT_BASE         0x600B1C00UL
#define LP_WDT_CONFIG0      (*(volatile uint32_t *)(LP_WDT_BASE + 0x00))
#define LP_WDT_CONFIG1      (*(volatile uint32_t *)(LP_WDT_BASE + 0x04))
#define LP_WDT_FEED         (*(volatile uint32_t *)(LP_WDT_BASE + 0x14))
#define LP_WDT_WPROTECT     (*(volatile uint32_t *)(LP_WDT_BASE + 0x18))
#define LP_WDT_WRITE_KEY    0x50D83AA1UL

#define LP_WDT_STG0_SHIFT       28
#define LP_WDT_STG0_MASK        (0x7UL << LP_WDT_STG0_SHIFT)
#define LP_WDT_STG0_RTC_RST     (0x4UL << LP_WDT_STG0_SHIFT)
#define LP_WDT_EN               (1UL << 31)
#define LP_WDT_RESET_TICKS      100UL

#define ME_WDT_TIMEOUT_MS   15000

// ─── MEMORY IPC INTER-CORE SHARED SEGMENT ─────────────────────────────────────
volatile uint32_t me_is_enabled  = 1;
volatile uint32_t me_hp_is_awake = 0;
volatile uint32_t me_boot_reason = ME_BOOT_REASON_NONE;
volatile uint32_t me_wdt_counter = 0;

volatile uint32_t me_cpu_load       = 0;
volatile uint32_t me_max_freq       = 80;
volatile uint32_t me_target_freq    = 80;
volatile uint32_t me_temperature    = 0;
volatile uint32_t me_throttle_temp  = 55; // Dynamic: thermal throttle threshold fetched from NVRAM (Default: 55)
volatile uint32_t me_emergency_temp = 75; // Dynamic: emergency thermal shutdown threshold fetched from NVRAM (Default: 75)

// ─── HARDWARE RESET PROPAGATION VIA LP WDT ────────────────────────────────────
static void me_hw_reset(uint32_t reason) {
    me_boot_reason = reason;
    ulp_lp_core_delay_us(1000);

    LP_WDT_WPROTECT = LP_WDT_WRITE_KEY;
    LP_WDT_FEED = (1UL << 31);
    LP_WDT_CONFIG1 = LP_WDT_RESET_TICKS;

    uint32_t cfg = LP_WDT_CONFIG0;
    cfg &= ~LP_WDT_STG0_MASK;
    cfg |= LP_WDT_STG0_RTC_RST;
    cfg |= LP_WDT_EN;
    LP_WDT_CONFIG0 = cfg;

    LP_WDT_WPROTECT = 0;
    while (1) { __asm__ volatile("nop"); }
}

int main(void) {
    ulp_lp_core_gpio_init(PIN_BTN_GND);
    ulp_lp_core_gpio_output_enable(PIN_BTN_GND);
    ulp_lp_core_gpio_set_level(PIN_BTN_GND, 0);

    ulp_lp_core_gpio_init(PIN_BTN_SENSE);
    ulp_lp_core_gpio_input_enable(PIN_BTN_SENSE);
    ulp_lp_core_gpio_pullup_enable(PIN_BTN_SENSE);

    uint32_t pwr_press_timer  = 0;
    uint32_t wdt_last_counter = 0;
    uint32_t wdt_stale_ms     = 0;
    uint32_t sched_loop_timer = 0;
    uint32_t me_uptime_ms     = 0;

    while (1) {
        if (me_is_enabled == 0) {
            ulp_lp_core_delay_us(100000);
            continue;
        }

        me_uptime_ms += 10;

        // 1. INTELLIGENT POWER BUTTON MANAGEMENT
        if (ulp_lp_core_gpio_get_level(PIN_BTN_SENSE) == 0) {
            if (pwr_press_timer < 10000) pwr_press_timer += 10;
            if (me_hp_is_awake == 1 && pwr_press_timer >= 5000) {
                me_hw_reset(ME_BOOT_REASON_FORCE_RESET);
            }
        } else {
            if (pwr_press_timer > 0) {
                if (me_hp_is_awake == 0) {
                    if (pwr_press_timer >= 3000) {
                        me_boot_reason = ME_BOOT_REASON_SETUP;
                        ulp_lp_core_wakeup_main_processor();
                    } else if (pwr_press_timer >= 50) {
                        me_boot_reason = ME_BOOT_REASON_NORMAL;
                        ulp_lp_core_wakeup_main_processor();
                    }
                }
                pwr_press_timer = 0;
            }
        }

        // 2. MAIN PROCESSOR SOFTWARE WATCHDOG DECODER
        if (me_hp_is_awake == 1) {
            if (me_wdt_counter != wdt_last_counter) {
                wdt_last_counter = me_wdt_counter;
                wdt_stale_ms = 0;
            } else {
                wdt_stale_ms += 10;
                if (wdt_stale_ms >= ME_WDT_TIMEOUT_MS) {
                    me_hw_reset(ME_BOOT_REASON_WDT);
                }
            }
        } else {
            wdt_last_counter = me_wdt_counter;
            wdt_stale_ms = 0;
        }

        // 3. SCHEDUTIL FREQUENCY SCALER & THERMAL SHIELD
        sched_loop_timer += 10;
        if (sched_loop_timer >= 100) {
            sched_loop_timer = 0;

            // Emergency thermal shutdown (threshold is NVRAM dynamic config)
            if (me_uptime_ms > 3000 && me_temperature >= me_emergency_temp && me_hp_is_awake == 1) {
                me_hw_reset(ME_BOOT_REASON_THERMAL);
            }

            // SchedUtil load-adaptive dynamic governor calculation
            uint32_t target = 80;
            if (me_temperature >= me_throttle_temp) {
                // Heavy thermal throttling enforced (throttle threshold is NVRAM dynamic config)
                target = 80;
            } else {
                if (me_cpu_load > 75) target = 160;
                else if (me_cpu_load > 30) target = 120;
                else target = 80;
            }

            if (target > me_max_freq) target = me_max_freq;
            me_target_freq = target;
        }

        ulp_lp_core_delay_us(10000);
    }
    return 0;
}
