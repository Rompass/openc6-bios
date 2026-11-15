#include "hal_wdt.h"

static uint32_t s_saved_timg0_wdt_cfg = 0;
static uint32_t s_saved_timg1_wdt_cfg = 0;

void hal_wdt_timg_save_and_disable(void)
{
    *C6_TIMG0_WDTWPROTECT_REG = C6_TIMG_WDT_KEY;
    s_saved_timg0_wdt_cfg = *C6_TIMG0_WDTCONFIG0_REG;
    *C6_TIMG0_WDTCONFIG0_REG = 0;
    *C6_TIMG0_WDTFEED_REG = 1;
    *C6_TIMG0_WDTWPROTECT_REG = 0;

    *C6_TIMG1_WDTWPROTECT_REG = C6_TIMG_WDT_KEY;
    s_saved_timg1_wdt_cfg = *C6_TIMG1_WDTCONFIG0_REG;
    *C6_TIMG1_WDTCONFIG0_REG = 0;
    *C6_TIMG1_WDTFEED_REG = 1;
    *C6_TIMG1_WDTWPROTECT_REG = 0;
}

void hal_wdt_timg_restore(void)
{
    *C6_TIMG0_WDTWPROTECT_REG = C6_TIMG_WDT_KEY;
    *C6_TIMG0_WDTFEED_REG = 1;
    *C6_TIMG0_WDTCONFIG0_REG = s_saved_timg0_wdt_cfg;
    *C6_TIMG0_WDTWPROTECT_REG = 0;

    *C6_TIMG1_WDTWPROTECT_REG = C6_TIMG_WDT_KEY;
    *C6_TIMG1_WDTFEED_REG = 1;
    *C6_TIMG1_WDTCONFIG0_REG = s_saved_timg1_wdt_cfg;
    *C6_TIMG1_WDTWPROTECT_REG = 0;
}

void hal_wdt_timg_clear_and_halt(void)
{
    *C6_TIMG0_WDTWPROTECT_REG = C6_TIMG_WDT_KEY;
    *C6_TIMG0_INT_CLR_REG = C6_TIMG_WDT_INT_CLR_BIT;
    *C6_TIMG0_WDTFEED_REG = 1;
    *C6_TIMG0_WDTCONFIG0_REG = 0;
    *C6_TIMG0_WDTWPROTECT_REG = 0;

    *C6_TIMG1_WDTWPROTECT_REG = C6_TIMG_WDT_KEY;
    *C6_TIMG1_INT_CLR_REG = C6_TIMG_WDT_INT_CLR_BIT;
    *C6_TIMG1_WDTFEED_REG = 1;
    *C6_TIMG1_WDTCONFIG0_REG = 0;
    *C6_TIMG1_WDTWPROTECT_REG = 0;
}
