#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    HAL_BOD_STRICT   = 0,
    HAL_BOD_RELAXED  = 1,
    HAL_BOD_DISABLED = 2
} hal_bod_level_t;

void     hal_power_set_bod(hal_bod_level_t level);

bool     hal_power_is_cold_boot(void);
int      hal_power_get_reset_reason(void);
bool     hal_power_is_panic_reset(void);
bool     hal_power_is_sw_or_sleep_reset(void);

uint32_t hal_power_measure_wake_hold_ms(void);
void     hal_power_init_runtime_pins(void);
bool     hal_power_check_clear_cmos_jumper(void);

void     hal_power_boot_btn_init(void);
bool     hal_power_is_boot_btn_pressed(void);

void     hal_power_enter_s5(bool enable_ulp_wakeup) __attribute__((noreturn));
void     hal_power_reboot(void) __attribute__((noreturn));
