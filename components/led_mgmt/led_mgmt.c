#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal_ws2812.h"
#include "led_mgmt.h"
#include "nvram.h"

static TaskHandle_t s_aura_task_handle = NULL;

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

static void aura_effect_task(void *arg)
{
    uint16_t hue = 0;
    uint8_t r, g, b;
    uint8_t brightness = 128;
    nvram_get_aura_brightness(&brightness);

    uint32_t nvs_check_counter = 0;

    while (1) {
        hal_ws2812_unlock();

        /* Poll NVRAM brightness only once every 2 seconds instead of every 30ms */
        if (++nvs_check_counter >= 66) {
            nvs_check_counter = 0;
            nvram_get_aura_brightness(&brightness);
        }

        hsv_to_rgb(hue, 255, brightness, &r, &g, &b);
        hal_ws2812_write_rgb(r, g, b);

        hue = (hue + 1) % 360;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

void led_mgmt_init(void)
{
    hal_ws2812_init();

    /* Test RGB sequence */
    hal_ws2812_write_rgb(255, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    hal_ws2812_write_rgb(0, 255, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    hal_ws2812_write_rgb(0, 0, 255);
    vTaskDelay(pdMS_TO_TICKS(100));
    led_mgmt_set_color(0, 0, 0);
}

void led_mgmt_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    /* Always unlock pad before sending new color data */
    hal_ws2812_unlock();

    if (s_aura_task_handle != NULL) {
        return;
    }

    uint8_t brightness = 255;
    nvram_get_aura_brightness(&brightness);

    uint8_t br_r = (uint8_t)(((uint32_t)r * brightness) / 255);
    uint8_t br_g = (uint8_t)(((uint32_t)g * brightness) / 255);
    uint8_t br_b = (uint8_t)(((uint32_t)b * brightness) / 255);

    hal_ws2812_write_rgb(br_r, br_g, br_b);

    /* If turning off completely (e.g. going to sleep), lock pin at 0V (GND) */
    if (r == 0 && g == 0 && b == 0) {
        hal_ws2812_latch_low();
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
