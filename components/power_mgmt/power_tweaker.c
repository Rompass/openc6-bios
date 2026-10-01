#include <stdio.h>
#include "power_tweaker.h"
#include "esp_pm.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Official APIs for interfacing with the Brownout Detector */
#include "esp_private/brownout.h"
#include "hal/brownout_hal.h"

static const char *TAG = "AI_TWEAKER";

extern void power_governor_start(void);

void power_tweaker_apply_bios_settings(void)
{
    cpu_freq_t freq;
    cpu_governor_t gov;
    bod_level_t bod;

    nvram_get_cpu_freq(&freq);
    nvram_get_cpu_governor(&gov);
    nvram_get_bod_level(&bod);

    ESP_LOGI(TAG, "=== APPLYING HARDWARE TWEAKS ===");

    /* Mute verbose ESP-IDF PM framework logs */
    esp_log_level_set("pm", ESP_LOG_WARN);

    /* ─── 1. FREQUENCY AND GOVERNOR REGULATION ────────────────────────────── */
    if (gov == GOV_DYNAMIC) {
        ESP_LOGI(TAG, "CPU Governor: DYNAMIC (Autonomous Hardware SchedUtil on LP-Core, Max: %d MHz)", freq);
    } else {
        ESP_LOGI(TAG, "CPU Governor: FIXED (Hardware Locked at %d MHz)", freq);
    }

    /* Launch junction temperature telemetry and autonomous SchedUtil on LP-Core */
    power_governor_start();

    /* ─── 2. BROWN-OUT DETECTOR (BOD) HARDWARE REGULATOR ─────────────────── */
    if (bod == BOD_DISABLED) {
        esp_brownout_disable();
        ESP_LOGW(TAG, "BOD: PHYSICAL PROTECTION DISABLED!");
    } else {
        brownout_hal_config_t bod_cfg = {
            .enabled = true,
            .reset_enabled = true,
            .flash_power_down = true,
            .rf_power_down = true
        };

        if (bod == BOD_STRICT) {
            bod_cfg.threshold = 7;
            ESP_LOGI(TAG, "BOD Level: STRICT (Threshold 7, ~2.8V)");
        } else {
            bod_cfg.threshold = 4;
            ESP_LOGI(TAG, "BOD Level: RELAXED (Threshold 4, ~2.5V)");
        }

        brownout_hal_config(&bod_cfg);
    }

    ESP_LOGI(TAG, "=================================");
}
