#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_mgmt.h"
#include "led_strip.h"
#include "esp_log.h"
#include "bios_core.h"
#include "nvram.h"

static const char *TAG = "LED_MGMT";
static led_strip_handle_t led_strip;
static TaskHandle_t aura_task_handle = NULL;

/**
 * @brief Helper function to convert HSV to RGB color space.
 * Enables smooth color cycling (via Hue) and brightness adjustments (via Value).
 */
static void hsv_to_rgb(uint32_t h, uint32_t s, uint32_t v, uint8_t *r, uint8_t *g, uint8_t *b) {
    h %= 360;
    uint32_t rgb_max = v;
    uint32_t rgb_min = rgb_max * (255 - s) / 255;
    uint32_t i = h / 60;
    uint32_t diff = h % 60;
    uint32_t rgb_adj = (rgb_max - rgb_min) * diff / 60;

    switch (i) {
        case 0: *r = rgb_max; *g = rgb_min + rgb_adj; *b = rgb_min; break;
        case 1: *r = rgb_max - rgb_adj; *g = rgb_max; *b = rgb_min; break;
        case 2: *r = rgb_min; *g = rgb_max; *b = rgb_min + rgb_adj; break;
        case 3: *r = rgb_min; *g = rgb_max - rgb_adj; *b = rgb_max; break;
        case 4: *r = rgb_min + rgb_adj; *g = rgb_min; *b = rgb_max; break;
        default: *r = rgb_max; *g = rgb_min; *b = rgb_max - rgb_adj; break;
    }
}

/**
 * @brief Background RTOS task responsible for handling the Aura Sync rainbow flow effect.
 */
static void aura_effect_task(void *arg) {
    uint16_t hue = 0;
    uint8_t r, g, b;
    uint8_t brightness;

    while (1) {
        // Retrieve current brightness configuration from NVRAM
        nvram_get_aura_brightness(&brightness);

        // Generate active color parameters: Saturation is set to 255 (max), Value corresponds to current brightness
        hsv_to_rgb(hue, 255, brightness, &r, &g, &b);

        // Map to GBR (Green, Blue, Red) physical pinout layout
        led_strip_set_pixel(led_strip, 0, g, b, r);
        led_strip_refresh(led_strip);

        hue = (hue + 1) % 360; // Increment and cycle the hue value
        vTaskDelay(pdMS_TO_TICKS(30)); // Control animation cycle speed
    }
}

void led_mgmt_init(void) {
    led_strip_config_t strip_config = {
        .strip_gpio_num = PIN_POST_LED,
        .max_leds = 1,
    };

    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
    };

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    led_strip_clear(led_strip);
    ESP_LOGI(TAG, "LED Strip initialized (GBR mode supported)");
}

/**
 * @brief Enables or disables the background Aura effect.
 */
void led_mgmt_set_aura_mode(aura_mode_t mode) {
    if (mode == AURA_RAINBOW) {
        if (aura_task_handle == NULL) {
            ESP_LOGI(TAG, "Starting Aura Rainbow task...");
            xTaskCreate(aura_effect_task, "aura_task", 2048, NULL, 5, &aura_task_handle);
        }
    } else {
        if (aura_task_handle != NULL) {
            ESP_LOGI(TAG, "Stopping Aura task.");
            vTaskDelete(aura_task_handle);
            aura_task_handle = NULL;
            led_strip_clear(led_strip);
        }
    }
}

/**
 * @brief Applies a static color scaled against the current system brightness setting.
 */
void led_mgmt_set_color(uint8_t r, uint8_t g, uint8_t b) {
    if (!led_strip) return;

    // If the active Aura task is running, it retains exclusive LED control; ignore static color overrides
    if (aura_task_handle != NULL) return;

    uint8_t brightness;
    nvram_get_aura_brightness(&brightness);

    // Scale static color intensity relative to NVRAM brightness limit
    uint8_t br_r = (r * brightness) / 255;
    uint8_t br_g = (g * brightness) / 255;
    uint8_t br_b = (b * brightness) / 255;

    // Apply GBR channel re-routing
    led_strip_set_pixel(led_strip, 0, br_g, br_b, br_r);
    led_strip_refresh(led_strip);
}

void led_mgmt_blink_post(uint8_t r, uint8_t g, uint8_t b, int count) {
    post_led_mode_t mode;
    nvram_get_post_led_mode(&mode);
    if (mode == POST_LED_DISABLED) return;

    // Aura task is bypassed during POST execution to avoid interfering with diagnostic blinks
    for (int i = 0; i < count; i++) {
        led_mgmt_set_color(r, g, b);
        vTaskDelay(pdMS_TO_TICKS(100));
        led_strip_clear(led_strip);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
