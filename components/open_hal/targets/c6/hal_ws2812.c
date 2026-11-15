#include "hal_ws2812.h"
#include "hal_board.h"
#include "hal_gpio.h"
#include "hal_clock.h"
#include "hal_cpu.h"
#include "hal_timer.h"
#include "esp_attr.h"
#include "driver/gpio.h"

static inline __attribute__((always_inline)) void ws2812_delay_cycles(volatile uint32_t cycles)
{
    while (cycles--) {
        __asm__ volatile ("nop");
    }
}

void hal_ws2812_init(void)
{
    /* Release any active pad hold from previous deep sleep */
    gpio_hold_dis((gpio_num_t)HAL_PIN_WS2812);

    gpio_reset_pin((gpio_num_t)HAL_PIN_WS2812);

    /* Opt-out from automatic sleep reconfiguration: prevents sleep manager from driving pin HIGH */
    gpio_sleep_sel_dis((gpio_num_t)HAL_PIN_WS2812);

    /* Disable internal strapping pull-up and enable active pull-down */
    gpio_pullup_dis((gpio_num_t)HAL_PIN_WS2812);
    gpio_pulldown_en((gpio_num_t)HAL_PIN_WS2812);

    *C6_GPIO_ENABLE_W1TS_REG = (1UL << HAL_PIN_WS2812);
    *C6_GPIO_OUT_W1TC_REG = (1UL << HAL_PIN_WS2812);
}

void hal_ws2812_unlock(void)
{
    gpio_hold_dis((gpio_num_t)HAL_PIN_WS2812);
}

IRAM_ATTR void hal_ws2812_write_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t bytes[3];
    bytes[0] = r;
    bytes[1] = g;
    bytes[2] = b;

    uint32_t mask = (1UL << HAL_PIN_WS2812);

    /* Read actual hardware CPU clock directly from PCR register: zero desync with LP-Core */
    uint32_t cpu_mhz = hal_clock_get_cpu_mhz();
    uint32_t t0h, t0l, t1h, t1l;

    if (cpu_mhz >= 160) {
        /* 160 MHz: divider 3 (HS_DIV=0, 120M_FORCE=0) */
        t0h = 4; t0l = 12; t1h = 12; t1l = 5;
    } else if (cpu_mhz >= 120) {
        /* 120 MHz: force_120m == 1 */
        t0h = 3; t0l = 9; t1h = 9; t1l = 4;
    } else {
        /* 80 MHz: divider 6 (HS_DIV=1, 120M_FORCE=0) */
        t0h = 1; t0l = 6; t1h = 6; t1l = 2;
    }

    uint32_t old_mstatus = hal_cpu_irq_disable();

    for (int i = 0; i < 3; i++) {
        uint8_t byte = bytes[i];
        for (int bit = 7; bit >= 0; bit--) {
            if (byte & (1 << bit)) {
                *C6_GPIO_OUT_W1TS_REG = mask;
                ws2812_delay_cycles(t1h);
                *C6_GPIO_OUT_W1TC_REG = mask;
                ws2812_delay_cycles(t1l);
            } else {
                *C6_GPIO_OUT_W1TS_REG = mask;
                ws2812_delay_cycles(t0h);
                *C6_GPIO_OUT_W1TC_REG = mask;
                ws2812_delay_cycles(t0l);
            }
        }
    }

    *C6_GPIO_OUT_W1TC_REG = mask;
    hal_timer_delay_us(300);

    hal_cpu_irq_restore(old_mstatus);
}

void hal_ws2812_latch_low(void)
{
    gpio_set_direction((gpio_num_t)HAL_PIN_WS2812, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)HAL_PIN_WS2812, 0);
    gpio_hold_en((gpio_num_t)HAL_PIN_WS2812);
}
