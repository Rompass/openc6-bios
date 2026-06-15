#include <stdio.h>
#include "power_tweaker.h"
#include "esp_pm.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Official APIs for interfacing with the Brownout Detector
#include "esp_private/brownout.h"
#include "hal/brownout_hal.h" // <-- Required for register-level hardware manipulation

static const char *TAG = "AI_TWEAKER";

// Export the subsystem initialization interface from power_governor.c
extern void power_governor_start(void);

void power_tweaker_apply_bios_settings(void) {
    cpu_freq_t freq;
    cpu_governor_t gov;
    bod_level_t bod;

    nvram_get_cpu_freq(&freq);
    nvram_get_cpu_governor(&gov);
    nvram_get_bod_level(&bod);

    ESP_LOGI(TAG, "=== APPLYING HARDWARE TWEAKS ===");

    // --- 1. FREQUENCY AND GOVERNOR REGULATION ---
    #if CONFIG_PM_ENABLE
    if (gov == GOV_DYNAMIC) {
        ESP_LOGI(TAG, "CPU Governor: DYNAMIC");
    } else {
        // Locked Performance Mode Profile
        esp_pm_config_t pm_config = {
            .max_freq_mhz = freq,
            .min_freq_mhz = freq,
            .light_sleep_enable = false
        };
        esp_pm_configure(&pm_config);
        ESP_LOGI(TAG, "CPU Clock locked at %d MHz", freq);
    }

    // Launch junction temperature monitoring and Management Engine watchdog services
    power_governor_start();
    #endif

    // --- 2. BROWN-OUT DETECTOR (BOD) HARDWARE REGULATOR ---
    if (bod == BOD_DISABLED) {
        esp_brownout_disable();
        ESP_LOGW(TAG, "BOD: PHYSICAL PROTECTION DISABLED!");
    } else {
        // Initialize baseline configuration parameters for the Brownout subsystem
        brownout_hal_config_t bod_cfg = {
            .enabled = true,
            .reset_enabled = true,     // Force hardware chip reset upon critical undervoltage
            .flash_power_down = true,  // Power down Flash during dropouts to safeguard metadata
            .rf_power_down = true      // Disable radio circuitry instantly during power dropouts
        };

        if (bod == BOD_STRICT) {
            // Strict Profile: Level 7 (~2.8V). Provides absolute protection against flash write corruption.
            bod_cfg.threshold = 7;
            ESP_LOGI(TAG, "BOD Level: STRICT (Threshold 7, ~2.8V)");
        } else { // BOD_RELAXED
            // Relaxed Profile: Level 4 (~2.5V). Tolerates momentary transients during Wi-Fi tx start
            // or allows deeper battery discharge levels before reset occurs.
            bod_cfg.threshold = 4;
            ESP_LOGI(TAG, "BOD Level: RELAXED (Threshold 4, ~2.5V)");
        }

        // Apply physical configuration parameters directly to LP_AON_BROWN_OUT_REG hardware registers
        brownout_hal_config(&bod_cfg);
    }

    ESP_LOGI(TAG, "=================================");
}
