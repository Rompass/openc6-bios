#include "hal_power.h"
#include "hal_board.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_private/brownout.h"
#include "hal/brownout_hal.h"

void hal_power_set_bod(hal_bod_level_t level)
{
    if (level == HAL_BOD_DISABLED) {
        esp_brownout_disable();
        return;
    }

    brownout_hal_config_t bod_cfg = {
        .enabled = true,
        .reset_enabled = true,
        .flash_power_down = true,
        .rf_power_down = true,
        .threshold = (level == HAL_BOD_STRICT) ? 7 : 4
    };
    brownout_hal_config(&bod_cfg);
}

bool hal_power_is_cold_boot(void)
{
    return esp_reset_reason() != ESP_RST_DEEPSLEEP;
}

int hal_power_get_reset_reason(void)
{
    return (int)esp_reset_reason();
}

bool hal_power_is_panic_reset(void)
{
    return esp_reset_reason() == ESP_RST_PANIC;
}

bool hal_power_is_sw_or_sleep_reset(void)
{
    esp_reset_reason_t rst = esp_reset_reason();
    return (rst == ESP_RST_SW || rst == ESP_RST_DEEPSLEEP);
}

uint32_t hal_power_measure_wake_hold_ms(void)
{
    uint32_t hold_ms = 0;

    if (!hal_power_is_cold_boot() &&
        (esp_sleep_get_wakeup_causes() & (1ULL << ESP_SLEEP_WAKEUP_EXT1))) {

        gpio_reset_pin((gpio_num_t)HAL_PIN_PWR_BTN_GND);
    gpio_set_direction((gpio_num_t)HAL_PIN_PWR_BTN_GND, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)HAL_PIN_PWR_BTN_GND, 0);

    gpio_reset_pin((gpio_num_t)HAL_PIN_PWR_BTN_SENSE);
    gpio_set_direction((gpio_num_t)HAL_PIN_PWR_BTN_SENSE, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)HAL_PIN_PWR_BTN_SENSE, GPIO_PULLUP_ONLY);

    while (gpio_get_level((gpio_num_t)HAL_PIN_PWR_BTN_SENSE) == 0 && hold_ms < 3000) {
        vTaskDelay(pdMS_TO_TICKS(10));
        hold_ms += 10;
    }
        }

        return hold_ms;
}

void hal_power_init_runtime_pins(void)
{
    /* Release hold latches and lock PWR_BTN_GND as strong 0V GND during runtime */
    gpio_hold_dis((gpio_num_t)HAL_PIN_PWR_BTN_GND);
    gpio_reset_pin((gpio_num_t)HAL_PIN_PWR_BTN_GND);
    gpio_set_direction((gpio_num_t)HAL_PIN_PWR_BTN_GND, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)HAL_PIN_PWR_BTN_GND, 0);

    /* Enforce internal pullup on power sense line */
    gpio_reset_pin((gpio_num_t)HAL_PIN_PWR_BTN_SENSE);
    gpio_set_direction((gpio_num_t)HAL_PIN_PWR_BTN_SENSE, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)HAL_PIN_PWR_BTN_SENSE, GPIO_PULLUP_ONLY);

    hal_power_boot_btn_init();
}

bool hal_power_check_clear_cmos_jumper(void)
{
    gpio_reset_pin((gpio_num_t)HAL_PIN_CMOS_GND);
    gpio_set_direction((gpio_num_t)HAL_PIN_CMOS_GND, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)HAL_PIN_CMOS_GND, 0);

    gpio_reset_pin((gpio_num_t)HAL_PIN_CLEAR_NVRAM);
    gpio_set_direction((gpio_num_t)HAL_PIN_CLEAR_NVRAM, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)HAL_PIN_CLEAR_NVRAM, GPIO_PULLUP_ONLY);

    vTaskDelay(pdMS_TO_TICKS(10));

    bool jumper_shorted = (gpio_get_level((gpio_num_t)HAL_PIN_CLEAR_NVRAM) == 0);
    gpio_set_direction((gpio_num_t)HAL_PIN_CMOS_GND, GPIO_MODE_DISABLE);
    return jumper_shorted;
}

void hal_power_boot_btn_init(void)
{
    gpio_reset_pin((gpio_num_t)HAL_PIN_BOOT_BTN);
    gpio_set_direction((gpio_num_t)HAL_PIN_BOOT_BTN, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)HAL_PIN_BOOT_BTN, GPIO_PULLUP_ONLY);
}

bool hal_power_is_boot_btn_pressed(void)
{
    return gpio_get_level((gpio_num_t)HAL_PIN_BOOT_BTN) == 0;
}

void hal_power_enter_s5(bool enable_ulp_wakeup)
{
    /* Virtual ground locked to 0V across deep sleep */
    gpio_reset_pin((gpio_num_t)HAL_PIN_PWR_BTN_GND);
    gpio_set_direction((gpio_num_t)HAL_PIN_PWR_BTN_GND, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)HAL_PIN_PWR_BTN_GND, 0);
    gpio_hold_en((gpio_num_t)HAL_PIN_PWR_BTN_GND);

    /* Sense input with pullup enabled */
    gpio_reset_pin((gpio_num_t)HAL_PIN_PWR_BTN_SENSE);
    gpio_set_direction((gpio_num_t)HAL_PIN_PWR_BTN_SENSE, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)HAL_PIN_PWR_BTN_SENSE, GPIO_PULLUP_ONLY);

    /* Hardware wakeup trigger via EXT1 on active-low sense line */
    esp_sleep_enable_ext1_wakeup(1ULL << HAL_PIN_PWR_BTN_SENSE, ESP_EXT1_WAKEUP_ANY_LOW);

    if (enable_ulp_wakeup) {
        esp_sleep_enable_ulp_wakeup();
    }

    esp_deep_sleep_start();
    while (1) {}
}

void hal_power_reboot(void)
{
    esp_restart();
    while (1) {}
}
