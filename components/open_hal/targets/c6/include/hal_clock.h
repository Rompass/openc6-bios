#pragma once

#include <stdint.h>

/*
 * ============================================================================
 * ESP32-C6 Hardware Clock / PCR Abstraction
 * Safe for both HP-Core and LP-Core builds.
 * ============================================================================
 */

/* ESP32-C6 TRM Rev 1.2, Ch. 8.4: CPU frequency configuration register */
#define C6_PCR_CPU_FREQ_CONF_REG ((volatile uint32_t *)0x60096118UL)
#define C6_PCR_CPU_DIV_MASK      0x1FF00UL
#define C6_PCR_CPU_HS_DIV_SHIFT  8
#define C6_PCR_CPU_120M_FORCE    (1UL << 16)
#define HAL_CLOCK_LOW_MHZ        80U
#define HAL_CLOCK_MID_MHZ        120U
#define HAL_CLOCK_HIGH_MHZ       160U

/**
 * @brief Reads physical HP CPU clock directly from silicon PCR register.
 * Always inlined (IRAM-safe).
 *
 * @return Active frequency in MHz (80, 120, or 160).
 */
static inline __attribute__((always_inline)) uint32_t hal_clock_get_cpu_mhz(void)
{
    uint32_t pcr = *C6_PCR_CPU_FREQ_CONF_REG;

    if (pcr & C6_PCR_CPU_120M_FORCE) {
        return 120;
    } else if (((pcr >> C6_PCR_CPU_HS_DIV_SHIFT) & 0xFFUL) == 1UL) {
        return 80;
    } else {
        return 160;
    }
}

/**
 * @brief Directly programs HP CPU frequency divider via PCR register.
 *
 * @param freq_mhz Target frequency (80, 120, or 160 MHz).
 */
static inline __attribute__((always_inline)) void hal_clock_set_cpu_mhz_raw(uint32_t freq_mhz)
{
    uint32_t reg = *C6_PCR_CPU_FREQ_CONF_REG & ~C6_PCR_CPU_DIV_MASK;

    if (freq_mhz >= 160) {
        /* 160 MHz: div_num = 0, force_120m = 0 */
    } else if (freq_mhz >= 120) {
        /* 120 MHz: force_120m = 1 */
        reg |= C6_PCR_CPU_120M_FORCE;
    } else {
        /* 80 MHz: div_num = 1 (bits [15:8] = 1) */
        reg |= (1UL << C6_PCR_CPU_HS_DIV_SHIFT);
    }

    *C6_PCR_CPU_FREQ_CONF_REG = reg;
}

/**
 * @brief Programs HP CPU frequency divider and synchronizes ROM tick scale.
 *
 * @param freq_mhz Target frequency (80, 120, or 160 MHz).
 */
void hal_clock_set_cpu_mhz(uint32_t freq_mhz);
