#include <stdio.h>
#include "power_tweaker.h"
#include "esp_log.h"
#include "nvram.h"
#include "hal_board.h"
#include "hal_clock.h"
#include "hal_power.h"
#include "management_engine.h"

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
    if (HAL_CAP_LP_CORE && management_engine_is_active()) {
        if (gov == GOV_DYNAMIC) {
            ESP_LOGI(TAG, "CPU Governor: DYNAMIC (Autonomous Hardware SchedUtil on LP-Core, Max: %d MHz)", freq);
        } else {
            ESP_LOGI(TAG, "CPU Governor: FIXED (Hardware Locked at %d MHz)", freq);
        }

        /* Launch junction temperature telemetry and autonomous SchedUtil on LP-Core */
        power_governor_start();
    } else {
        ESP_LOGW(TAG, "LP-Core ME disabled: SchedUtil & Thermal Guard OFF. Locking CPU at %d MHz", freq);
        hal_clock_set_cpu_mhz((uint32_t)freq);
    }

    /* ─── 2. BROWN-OUT DETECTOR (BOD) HARDWARE REGULATOR ─────────────────── */
    #if HAL_CAP_BOD
    if (bod == BOD_DISABLED) {
        hal_power_set_bod(HAL_BOD_DISABLED);
        ESP_LOGW(TAG, "BOD: PHYSICAL PROTECTION DISABLED!");
    } else if (bod == BOD_STRICT) {
        hal_power_set_bod(HAL_BOD_STRICT);
        ESP_LOGI(TAG, "BOD Level: STRICT (Threshold 7, ~2.8V)");
    } else {
        hal_power_set_bod(HAL_BOD_RELAXED);
        ESP_LOGI(TAG, "BOD Level: RELAXED (Threshold 4, ~2.5V)");
    }
    #endif

    ESP_LOGI(TAG, "=================================");
}
