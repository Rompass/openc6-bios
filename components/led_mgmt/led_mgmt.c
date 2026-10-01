#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "rom/ets_sys.h"
#include "driver/gpio.h"
#include "led_mgmt.h"
#include "nvram.h"

#define WS2812_GPIO 8

/* ESP32-C6 TRM Rev 1.2, Ch. 6: Direct atomic GPIO registers (Base: 0x60091000) */
#define GPIO_OUT_W1TS_REG    ((volatile uint32_t *)0x60091008)
#define GPIO_OUT_W1TC_REG    ((volatile uint32_t *)0x6009100C)
#define GPIO_ENABLE_W1TS_REG ((volatile uint32_t *)0x60091024)

static TaskHandle_t s_aura_task_handle = NULL;

static inline void delay_cycles(volatile uint32_t cycles)
{
    while (cycles--) {
        __asm__ volatile ("nop");
    }
}

static void hsv_to_rgb(uint32_t h, uint32_t s, uint32_t v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    h %= 360;
    uint32_t rgb_max = v;
    uint32_t rgb_min = rgb_max * (255 - s) / 255;
    uint32_t i = h / 60;
    uint32_t diff = h % 60;
    uint32_t rgb_adj = (rgb_max - rgb_min) * diff / 60;

    switch (i) {
        case 0:  *r = rgb_max; *g = rgb_min + rgb_adj; *b = rgb_min; break;
        case 1:  *r = rgb_max - rgb_adj; *g = rgb_max; *b = rgb_min; break;
        case 2:  *r = rgb_min; *g = rgb_max; *b = rgb_min + rgb_adj; break;
        case 3:  *r = rgb_min; *g = rgb_max - rgb_adj; *b = rgb_max; break;
        case 4:  *r = rgb_min + rgb_adj; *g = rgb_min; *b = rgb_max; break;
        default: *r = rgb_max; *g = rgb_min; *b = rgb_max - rgb_adj; break;
    }
}

/**
 * @brief Bit-bangs 24-bit waveform to GPIO 8 directly via single-cycle atomic registers.
 * Dynamically scales loop cycles to match real-time PCR clock register state (80, 120, 160 MHz).
 * Disables interrupts temporarily to protect WS2812 sub-microsecond pulse timing from jitter.
 */
IRAM_ATTR static void ws2812_write_pixel(uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t bytes[3];
    bytes[0] = r;
    bytes[1] = g;
    bytes[2] = b;

    uint32_t mask = (1UL << WS2812_GPIO);

    /* Read actual hardware CPU clock directly from PCR register: zero desync with LP-Core */
    uint32_t pcr = *((volatile uint32_t *)0x60096118UL);
    uint32_t t0h, t0l, t1h, t1l;

    if ((pcr & 0x1FF00UL) == 0) {
        /* 160 MHz: divider 3 (HS_DIV=0, 120M_FORCE=0) */
        t0h = 4; t0l = 12; t1h = 12; t1l = 5;
    } else if (pcr & (1UL << 16)) {
        /* 120 MHz: force_120m == 1 */
        t0h = 3; t0l = 9; t1h = 9; t1l = 4;
    } else {
        /* 80 MHz: divider 6 (HS_DIV=1, 120M_FORCE=0) */
        t0h = 1; t0l = 6; t1h = 6; t1l = 2;
    }

    uint32_t old_mstatus;
    asm volatile ("csrrci %0, mstatus, 8" : "=r"(old_mstatus));

    for (int i = 0; i < 3; i++) {
        uint8_t byte = bytes[i];
        for (int bit = 7; bit >= 0; bit--) {
            if (byte & (1 << bit)) {
                *GPIO_OUT_W1TS_REG = mask;
                delay_cycles(t1h);
                *GPIO_OUT_W1TC_REG = mask;
                delay_cycles(t1l);
            } else {
                *GPIO_OUT_W1TS_REG = mask;
                delay_cycles(t0h);
                *GPIO_OUT_W1TC_REG = mask;
                delay_cycles(t0l);
            }
        }
    }

    *GPIO_OUT_W1TC_REG = mask;
    ets_delay_us(300);

    asm volatile ("csrw mstatus, %0" : : "r"(old_mstatus));
}

static void aura_effect_task(void *arg)
{
    uint16_t hue = 0;
    uint8_t r, g, b;
    uint8_t brightness = 128;
    nvram_get_aura_brightness(&brightness);

    uint32_t nvs_check_counter = 0;

    while (1) {
        gpio_hold_dis((gpio_num_t)WS2812_GPIO);

        /* Poll NVRAM brightness only once every 2 seconds instead of every 30ms */
        if (++nvs_check_counter >= 66) {
            nvs_check_counter = 0;
            nvram_get_aura_brightness(&brightness);
        }

        hsv_to_rgb(hue, 255, brightness, &r, &g, &b);
        ws2812_write_pixel(r, g, b);

        hue = (hue + 1) % 360;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

void led_mgmt_init(void)
{
    /* Release any active pad hold from previous deep sleep */
    gpio_hold_dis((gpio_num_t)WS2812_GPIO);

    gpio_reset_pin((gpio_num_t)WS2812_GPIO);

    /* Opt-out from automatic sleep reconfiguration: prevents sleep manager from driving pin HIGH */
    gpio_sleep_sel_dis((gpio_num_t)WS2812_GPIO);

    /* Disable internal strapping pull-up and enable active pull-down */
    gpio_pullup_dis((gpio_num_t)WS2812_GPIO);
    gpio_pulldown_en((gpio_num_t)WS2812_GPIO);

    *GPIO_ENABLE_W1TS_REG = (1UL << WS2812_GPIO);
    *GPIO_OUT_W1TC_REG = (1UL << WS2812_GPIO);

    /* Test RGB sequence */
    ws2812_write_pixel(255, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    ws2812_write_pixel(0, 255, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    ws2812_write_pixel(0, 0, 255);
    vTaskDelay(pdMS_TO_TICKS(100));
    led_mgmt_set_color(0, 0, 0);
}

void led_mgmt_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    /* Always unlock pad before sending new color data */
    gpio_hold_dis((gpio_num_t)WS2812_GPIO);

    if (s_aura_task_handle != NULL) {
        return;
    }

    uint8_t brightness = 255;
    nvram_get_aura_brightness(&brightness);

    uint8_t br_r = (uint8_t)(((uint32_t)r * brightness) / 255);
    uint8_t br_g = (uint8_t)(((uint32_t)g * brightness) / 255);
    uint8_t br_b = (uint8_t)(((uint32_t)b * brightness) / 255);

    ws2812_write_pixel(br_r, br_g, br_b);

    /* If turning off completely (e.g. going to sleep), lock pin at 0V (GND) */
    if (r == 0 && g == 0 && b == 0) {
        gpio_set_direction((gpio_num_t)WS2812_GPIO, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)WS2812_GPIO, 0);
        gpio_hold_en((gpio_num_t)WS2812_GPIO);
    }
}

void led_mgmt_set_aura_mode(aura_mode_t mode)
{
    if (mode == AURA_RAINBOW) {
        if (s_aura_task_handle == NULL) {
            xTaskCreate(aura_effect_task, "aura_task", 2048, NULL, 5, &s_aura_task_handle);
        }
    } else if (mode == AURA_STATIC) {
        if (s_aura_task_handle != NULL) {
            vTaskDelete(s_aura_task_handle);
            s_aura_task_handle = NULL;
        }
        /* Static Blue (COLOR_BIOS_SETUP) scaled by NVRAM brightness */
        led_mgmt_set_color(COLOR_BIOS_SETUP);
    } else {
        if (s_aura_task_handle != NULL) {
            vTaskDelete(s_aura_task_handle);
            s_aura_task_handle = NULL;
        }
        led_mgmt_set_color(0, 0, 0);
    }
}

void led_mgmt_apply_aura_from_nvram(void)
{
    aura_mode_t aura;
    nvram_get_aura_mode(&aura);
    led_mgmt_set_aura_mode(aura);
}

void led_mgmt_blink_post(uint8_t r, uint8_t g, uint8_t b, int count)
{
    post_led_mode_t mode;
    nvram_get_post_led_mode(&mode);
    if (mode == POST_LED_DISABLED) {
        return;
    }

    for (int i = 0; i < count; i++) {
        led_mgmt_set_color(r, g, b);
        vTaskDelay(pdMS_TO_TICKS(100));
        led_mgmt_set_color(0, 0, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
