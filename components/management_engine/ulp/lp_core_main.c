#include <stdint.h>
#include <stdbool.h>
#include "ulp_lp_core_utils.h"
#include "ulp_lp_core_gpio.h"
#include "hal_board.h"
#include "hal_clock.h"
#include "hal_wdt.h"
#include "me_shared.h"

#define ME_WDT_TIMEOUT_MS       15000

// ─── MEMORY IPC INTER-CORE SHARED SEGMENT ─────────────────────────────────────
volatile uint32_t me_is_enabled  = 1;
volatile uint32_t me_hp_is_awake = 0;
volatile uint32_t me_boot_reason = ME_BOOT_REASON_NONE;
volatile uint32_t me_wdt_counter = 0;

volatile uint32_t me_hp_busy_ticks = 0;
volatile uint32_t me_hp_idle_ticks = 0;
volatile uint32_t me_cpu_load       = 0;
volatile uint32_t me_max_freq       = HAL_CLOCK_LOW_MHZ;
volatile uint32_t me_min_freq       = HAL_CLOCK_LOW_MHZ;
volatile uint32_t me_target_freq    = HAL_CLOCK_LOW_MHZ;
volatile uint32_t me_temperature    = 0;
volatile uint32_t me_throttle_temp  = 55; /* Dynamic thermal throttle threshold from NVRAM */
volatile uint32_t me_emergency_temp = 75; /* Dynamic emergency thermal shutdown threshold from NVRAM */

// ─── HARDWARE RESET PROPAGATION VIA LP WDT ────────────────────────────────────
static void me_hw_reset(uint32_t reason) {
    me_boot_reason = reason;
    ulp_lp_core_delay_us(1000);
    hal_wdt_lp_trigger_reset();
}

int main(void) {
    ulp_lp_core_gpio_init(HAL_PIN_PWR_BTN_GND);
    ulp_lp_core_gpio_output_enable(HAL_PIN_PWR_BTN_GND);
    ulp_lp_core_gpio_set_level(HAL_PIN_PWR_BTN_GND, 0);

    ulp_lp_core_gpio_init(HAL_PIN_PWR_BTN_SENSE);
    ulp_lp_core_gpio_input_enable(HAL_PIN_PWR_BTN_SENSE);
    ulp_lp_core_gpio_pullup_enable(HAL_PIN_PWR_BTN_SENSE);

    uint32_t pwr_press_timer    = 0;
    uint32_t wdt_last_counter   = 0;
    uint32_t wdt_stale_ms       = 0;
    uint32_t sched_loop_timer   = 0;
    uint32_t me_uptime_ms       = 0;

    uint32_t last_busy_ticks    = 0;
    uint32_t last_idle_ticks    = 0;
    uint32_t current_applied    = HAL_CLOCK_LOW_MHZ;
    uint32_t downscale_hold_ms  = 0;

    /* Detect initial hardware CPU frequency from PCR state */
    uint32_t init_hw = hal_clock_get_cpu_mhz();
    if (init_hw == HAL_CLOCK_MID_MHZ || init_hw == HAL_CLOCK_LOW_MHZ) {
        current_applied = init_hw;
    } else {
        /* Bootloader left max clock: immediately drop hardware to power-save */
        current_applied = HAL_CLOCK_LOW_MHZ;
        hal_clock_set_cpu_mhz_raw(HAL_CLOCK_LOW_MHZ);
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
        if (ulp_lp_core_gpio_get_level(HAL_PIN_PWR_BTN_SENSE) == 0) {
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
            uint32_t target = HAL_CLOCK_LOW_MHZ;
            if (me_temperature >= me_throttle_temp) {
                /* Thermal throttle clamp: lock at min clock until temperature drops */
                target = HAL_CLOCK_LOW_MHZ;
            } else {
                if (me_cpu_load >= 70) {
                    target = HAL_CLOCK_HIGH_MHZ;
                } else if (me_cpu_load >= 20) {
                    target = HAL_CLOCK_MID_MHZ;
                } else {
                    target = HAL_CLOCK_LOW_MHZ;
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
                hal_clock_set_cpu_mhz_raw(current_applied);
                me_target_freq = current_applied;
            } else if (target < current_applied) {
                downscale_hold_ms += 100;
                if (downscale_hold_ms >= 1000) {
                    current_applied = target;
                    downscale_hold_ms = 0;
                    hal_clock_set_cpu_mhz_raw(current_applied);
                    me_target_freq = current_applied;
                }
            } else {
                downscale_hold_ms = 0;
                /* Enforce hardware frequency if external drivers altered PCR register behind ME */
                if (hal_clock_get_cpu_mhz() != current_applied) {
                    hal_clock_set_cpu_mhz_raw(current_applied);
                }
            }
        }

        ulp_lp_core_delay_us(10000);
    }
    return 0;
}
