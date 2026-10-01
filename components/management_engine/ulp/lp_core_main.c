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

// ─── ESP32-C6 HARDWARE CLOCK GOVERNOR REGISTERS ───────────────────────────────
/* ESP32-C6 TRM Rev 1.2, Ch. 8.4: Direct CPU frequency configuration register */
#define PCR_CPU_FREQ_CONF_REG (*(volatile uint32_t *)0x60096118UL)

/* LP_WDT control bits from lp_wdt_reg.h */
#define LP_WDT_CHIP_RESET_EN     (1UL << 8)
#define LP_WDT_PROCPU_RESET_EN   (1UL << 11)
#define LP_WDT_FLASHBOOT_EN      (1UL << 12)
#define LP_WDT_STG0_RTC_RST      (0x4UL << LP_WDT_STG0_SHIFT)

// ─── MEMORY IPC INTER-CORE SHARED SEGMENT ─────────────────────────────────────
volatile uint32_t me_is_enabled  = 1;
volatile uint32_t me_hp_is_awake = 0;
volatile uint32_t me_boot_reason = ME_BOOT_REASON_NONE;
volatile uint32_t me_wdt_counter = 0;

volatile uint32_t me_hp_busy_ticks = 0;
volatile uint32_t me_hp_idle_ticks = 0;
volatile uint32_t me_cpu_load       = 0;
volatile uint32_t me_max_freq       = 80;
volatile uint32_t me_min_freq       = 80;
volatile uint32_t me_target_freq    = 80;
volatile uint32_t me_temperature    = 0;
volatile uint32_t me_throttle_temp  = 55; /* Dynamic thermal throttle threshold from NVRAM */
volatile uint32_t me_emergency_temp = 75; /* Dynamic emergency thermal shutdown threshold from NVRAM */

// ─── HARDWARE RESET PROPAGATION VIA LP WDT ────────────────────────────────────
static void me_hw_reset(uint32_t reason) {
    me_boot_reason = reason;
    ulp_lp_core_delay_us(1000);

    /* Unlock write protection */
    LP_WDT_WPROTECT = LP_WDT_WRITE_KEY;

    /* Set Stage 0 timeout to 500 slow clock cycles (~3.3ms) for stable CDC sync */
    LP_WDT_CONFIG1 = 500UL;

    uint32_t cfg = LP_WDT_CONFIG0;
    cfg &= ~LP_WDT_STG0_MASK;
    cfg |= LP_WDT_STG0_RTC_RST;       /* Action 4: Full System + RTC Reset */
    cfg |= LP_WDT_PROCPU_RESET_EN;    /* BIT 11: Explicitly permit resetting CPU0 */
    cfg |= LP_WDT_CHIP_RESET_EN;      /* BIT 8: Trigger complete analog/digital chip reset */
    cfg |= LP_WDT_FLASHBOOT_EN;       /* BIT 12: Maintain watchdog protection across boot */
    cfg |= LP_WDT_EN;                 /* BIT 31: Enable watchdog */
    LP_WDT_CONFIG0 = cfg;

    /* Trigger immediate evaluation */
    LP_WDT_FEED = (1UL << 31);
    LP_WDT_WPROTECT = 0;

    /* Await instantaneous hardware reset execution */
    while (1) { __asm__ volatile("nop"); }
}

/**
 * @brief Directly configures HP CPU frequency divider via PCR registers from LP-Core.
 * Glitch-free clock multiplexer switches frequency synchronously in hardware.
 *
 * @param freq_mhz Target frequency (80, 120, or 160 MHz).
 */
static void me_set_cpu_freq_hardware(uint32_t freq_mhz)
{
    uint32_t reg = PCR_CPU_FREQ_CONF_REG;
    /* Mask out bits [16:8]: cpu_hs_div_num (bits 15:8) and cpu_hs_120m_force (bit 16) */
    reg &= ~0x1FF00UL;

    if (freq_mhz >= 160) {
        /* 160 MHz: divider 3 -> div_num = 0, force_120m = 0 */
    } else if (freq_mhz >= 120) {
        /* 120 MHz: divider 4 -> div_num = 0, force_120m = 1 */
        reg |= (1UL << 16);
    } else {
        /* 80 MHz: divider 6 -> div_num = 1 (bits 15:8 = 1), force_120m = 0 */
        reg |= (1UL << 8);
    }

    PCR_CPU_FREQ_CONF_REG = reg;
}

int main(void) {
    ulp_lp_core_gpio_init(PIN_BTN_GND);
    ulp_lp_core_gpio_output_enable(PIN_BTN_GND);
    ulp_lp_core_gpio_set_level(PIN_BTN_GND, 0);

    ulp_lp_core_gpio_init(PIN_BTN_SENSE);
    ulp_lp_core_gpio_input_enable(PIN_BTN_SENSE);
    ulp_lp_core_gpio_pullup_enable(PIN_BTN_SENSE);

    /* Initialize hardware CPU clock to energy-efficient 80 MHz setpoint upon boot */

    uint32_t pwr_press_timer    = 0;
    uint32_t wdt_last_counter   = 0;
    uint32_t wdt_stale_ms       = 0;
    uint32_t sched_loop_timer   = 0;
    uint32_t me_uptime_ms       = 0;

    uint32_t last_busy_ticks    = 0;
    uint32_t last_idle_ticks    = 0;
    uint32_t current_applied    = 80;
    uint32_t downscale_hold_ms  = 0;

    /* Detect initial hardware CPU frequency from PCR state */
    uint32_t init_pcr = PCR_CPU_FREQ_CONF_REG;
    if (init_pcr & (1UL << 16)) {
        current_applied = 120;
    } else if ((init_pcr >> 8) & 0xFF) {
        current_applied = 80;
    } else {
        /* Bootloader left 160 MHz: immediately drop hardware to 80 MHz power-save */
        current_applied = 80;
        me_set_cpu_freq_hardware(80);
    }
    me_target_freq = current_applied;
    downscale_hold_ms = 0;

    while (1) {
        if (me_is_enabled == 0) {
            ulp_lp_core_delay_us(100000);
            continue;
        }

        me_uptime_ms += 10;

        // 1. INTELLIGENT POWER BUTTON MANAGEMENT (Debounced)
        if (ulp_lp_core_gpio_get_level(PIN_BTN_SENSE) == 0) {
            if (pwr_press_timer < 10000) {
                pwr_press_timer += 10;
            }

            /* Hard reset: 3000ms unbroken contact while running */
            if (me_hp_is_awake == 1 && pwr_press_timer >= 3000) {
                me_hw_reset(ME_BOOT_REASON_FORCE_RESET);
            }
        } else {
            /* Button released */
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
                /* Leaky bucket debounce: prevent contact jitter from clearing timer instantly */
                if (pwr_press_timer > 30) {
                    pwr_press_timer -= 30;
                } else {
                    pwr_press_timer = 0;
                }
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

        // 3. AUTONOMOUS SCHEDUTIL GOVERNOR & HARDWARE PCR SCALER
        sched_loop_timer += 10;
        if (sched_loop_timer >= 100) {
            sched_loop_timer = 0;

            /* Emergency thermal shutdown: physical core temperature exceeds critical limit */
            if (me_uptime_ms > 3000 && me_temperature >= me_emergency_temp && me_hp_is_awake == 1) {
                me_hw_reset(ME_BOOT_REASON_THERMAL);
            }

            /* Calculate true CPU utilization across U-mode and kernel via FreeRTOS Tick Sampling */
            uint32_t cur_busy = me_hp_busy_ticks;
            uint32_t cur_idle = me_hp_idle_ticks;

            uint32_t delta_busy = cur_busy - last_busy_ticks;
            uint32_t delta_idle = cur_idle - last_idle_ticks;

            last_busy_ticks = cur_busy;
            last_idle_ticks = cur_idle;

            uint32_t total = delta_busy + delta_idle;
            if (total > 0) {
                me_cpu_load = (delta_busy * 100UL) / total;
            }

            /* Frequency target calculation */
            uint32_t target = 80;
            if (me_temperature >= me_throttle_temp) {
                /* Thermal throttle clamp: lock at 80 MHz until temperature drops */
                target = 80;
            } else {
                if (me_cpu_load >= 70) {
                    target = 160;
                } else if (me_cpu_load >= 20) {
                    target = 120;
                } else {
                    target = 80;
                }
            }

            if (target > me_max_freq) {
                target = me_max_freq;
            }
            if (target < me_min_freq) {
                target = me_min_freq;
            }

            /* Hysteresis control: Instant step-up on load spike, 1000ms hold on step-down */
            if (target > current_applied) {
                current_applied = target;
                downscale_hold_ms = 0;
                me_set_cpu_freq_hardware(current_applied);
                me_target_freq = current_applied;
            } else if (target < current_applied) {
                downscale_hold_ms += 100;
                if (downscale_hold_ms >= 1000) {
                    current_applied = target;
                    downscale_hold_ms = 0;
                    me_set_cpu_freq_hardware(current_applied);
                    me_target_freq = current_applied;
                }
            } else {
                downscale_hold_ms = 0;
                /* Enforce hardware frequency if external drivers altered PCR register behind ME */
                uint32_t live_pcr = PCR_CPU_FREQ_CONF_REG;
                uint32_t live_hw = (live_pcr & (1UL << 16)) ? 120 : (((live_pcr >> 8) & 0xFF) ? 80 : 160);
                if (live_hw != current_applied) {
                    me_set_cpu_freq_hardware(current_applied);
                }
            }
        }

        ulp_lp_core_delay_us(10000);
    }
    return 0;
}
