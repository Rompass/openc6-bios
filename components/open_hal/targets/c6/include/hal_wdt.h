#pragma once

#include <stdint.h>

/*
 * ============================================================================
 * ESP32-C6 Hardware Watchdog Registers (TIMG0/TIMG1 & LP_WDT)
 * ============================================================================
 */

/* ESP32-C6 TRM Rev 1.2: Timer Group Watchdog registers */
#define C6_TIMG0_WDTCONFIG0_REG  ((volatile uint32_t *)0x60008048UL)
#define C6_TIMG0_WDTFEED_REG     ((volatile uint32_t *)0x60008060UL)
#define C6_TIMG0_WDTWPROTECT_REG ((volatile uint32_t *)0x60008064UL)
#define C6_TIMG0_INT_CLR_REG     ((volatile uint32_t *)0x60008078UL)

#define C6_TIMG1_WDTCONFIG0_REG  ((volatile uint32_t *)0x60009048UL)
#define C6_TIMG1_WDTFEED_REG     ((volatile uint32_t *)0x60009060UL)
#define C6_TIMG1_WDTWPROTECT_REG ((volatile uint32_t *)0x60009064UL)
#define C6_TIMG1_INT_CLR_REG     ((volatile uint32_t *)0x60009078UL)

#define C6_TIMG_WDT_KEY          0x50D83AA1UL
#define C6_TIMG_WDT_INT_CLR_BIT  (1UL << 1)

/* ESP32-C6 LP Coprocessor Watchdog (LP_WDT) registers */
#define C6_LP_WDT_BASE           0x600B1C00UL
#define C6_LP_WDT_CONFIG0_REG    ((volatile uint32_t *)(C6_LP_WDT_BASE + 0x00))
#define C6_LP_WDT_CONFIG1_REG    ((volatile uint32_t *)(C6_LP_WDT_BASE + 0x04))
#define C6_LP_WDT_FEED_REG       ((volatile uint32_t *)(C6_LP_WDT_BASE + 0x14))
#define C6_LP_WDT_WPROTECT_REG   ((volatile uint32_t *)(C6_LP_WDT_BASE + 0x18))
#define C6_LP_WDT_KEY            0x50D83AA1UL

#define C6_LP_WDT_STG0_SHIFT     28
#define C6_LP_WDT_STG0_MASK      (0x7UL << C6_LP_WDT_STG0_SHIFT)
#define C6_LP_WDT_STG0_RTC_RST   (0x4UL << C6_LP_WDT_STG0_SHIFT)
#define C6_LP_WDT_CHIP_RESET_EN  (1UL << 8)
#define C6_LP_WDT_PROCPU_RST_EN  (1UL << 11)
#define C6_LP_WDT_FLASHBOOT_EN   (1UL << 12)
#define C6_LP_WDT_EN             (1UL << 31)

void hal_wdt_timg_save_and_disable(void);
void hal_wdt_timg_restore(void);
void hal_wdt_timg_clear_and_halt(void);

/**
 * @brief Triggers immediate full SoC reset via LP-Core Watchdog.
 */
static inline __attribute__((noreturn)) void hal_wdt_lp_trigger_reset(void)
{
    /* Unlock write protection */
    *C6_LP_WDT_WPROTECT_REG = C6_LP_WDT_KEY;

    /* Set Stage 0 timeout to 500 slow clock cycles (~3.3ms) for stable CDC sync */
    *C6_LP_WDT_CONFIG1_REG = 500UL;

    uint32_t cfg = *C6_LP_WDT_CONFIG0_REG;
    cfg &= ~C6_LP_WDT_STG0_MASK;
    cfg |= C6_LP_WDT_STG0_RTC_RST;   /* Action 4: Full System + RTC Reset */
    cfg |= C6_LP_WDT_PROCPU_RST_EN;  /* BIT 11: Explicitly permit resetting CPU0 */
    cfg |= C6_LP_WDT_CHIP_RESET_EN;  /* BIT 8: Trigger complete analog/digital chip reset */
    cfg |= C6_LP_WDT_FLASHBOOT_EN;   /* BIT 12: Maintain watchdog protection across boot */
    cfg |= C6_LP_WDT_EN;             /* BIT 31: Enable watchdog */
    *C6_LP_WDT_CONFIG0_REG = cfg;

    /* Trigger immediate evaluation */
    *C6_LP_WDT_FEED_REG = (1UL << 31);
    *C6_LP_WDT_WPROTECT_REG = 0;

    while (1) {
        __asm__ volatile ("nop");
    }
}
