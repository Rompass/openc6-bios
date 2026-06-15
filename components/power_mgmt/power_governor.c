#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_pm.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/temperature_sensor.h"
#include "nvram.h"
#include "me_shared.h"

static const char *TAG = "SCHEDUTIL";
static TaskHandle_t gov_task_handle = NULL;
static TaskHandle_t temp_task_handle = NULL;

extern bool is_me_enabled; // Fetch global ME state from bios_init.c

// ─── JUNCTION TEMPERATURE MONITORING TASK (Always Active) ─────────────────
static void openc6_temperature_task(void *arg) {
    temperature_sensor_handle_t temp_sensor = NULL;

    // Attempt 1: Default configuration for core operating range (20°C to 100°C)
    temperature_sensor_config_t temp_cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100);
    esp_err_t err = temperature_sensor_install(&temp_cfg, &temp_sensor);

    // Attempt 2: Fallback configuration range if high range fails calibration (-10°C to 80°C)
    if (err != ESP_OK) {
        temp_cfg.range_min = -10;
        temp_cfg.range_max = 80;
        err = temperature_sensor_install(&temp_cfg, &temp_sensor);
    }

    if (err == ESP_OK) {
        temperature_sensor_enable(temp_sensor);
        ESP_LOGI(TAG, "Hardware Temperature Sensor initialized successfully!");
    } else {
        ESP_LOGE(TAG, "Failed to init TSENS completely: %s", esp_err_to_name(err));
    }

    while (1) {
        if (temp_sensor != NULL) {
            float tsens_value = 0.0f;
            esp_err_t res = temperature_sensor_get_celsius(temp_sensor, &tsens_value);

            if (res == ESP_OK) {
                // Write calibrated metric directly to LP-core shared RAM
                ulp_me_temperature = (uint32_t)tsens_value;
            } else {
                ESP_LOGE(TAG, "Failed to read temp: %s", esp_err_to_name(res));
            }
        } else {
            ulp_me_temperature = 40; // Fallback dummy default temperature representation
        }

        // Poll junction temperature at 2-second intervals
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

// ─── GOVERNOR TASK (Active exclusively under DYNAMIC mode) ───────────────
static void openc6_schedutil_task(void *arg) {
    uint32_t last_idle_time = 0;
    uint32_t last_total_time = 0;
    uint32_t current_applied_freq = 0;

    cpu_freq_t max_bios_freq;
    nvram_get_cpu_freq(&max_bios_freq); // Fetch frequency upper limit from NVRAM for Legacy governor fallback

    while (1) {
        TaskStatus_t idle_stats;
        vTaskGetInfo(xTaskGetIdleTaskHandle(), &idle_stats, pdFALSE, eInvalid);

        uint32_t now = esp_timer_get_time(); // Time elapsed in microseconds
        uint32_t idle_now = idle_stats.ulRunTimeCounter;

        if (last_total_time > 0) {
            uint32_t delta_total = now - last_total_time;
            uint32_t delta_idle = idle_now - last_idle_time;

            // Calculate active CPU load: 0 to 100%
            uint32_t load = 0;
            if (delta_total > delta_idle) {
                load = 100 - ((delta_idle * 100) / delta_total);
            }

            uint32_t target_freq = 80;

            if (is_me_enabled) {
                // --- MANAGEMENT ENGINE ACTIVE (Thermal and SchedUtil delegation) ---
                ulp_me_cpu_load = load;
                target_freq = ulp_me_target_freq; // Fetch target calculated by the LP Coprocessor
            } else {
                // --- LEGACY MODE (Management Engine Disabled) ---
                if (load > 75) target_freq = 160;
                else if (load > 30) target_freq = 120;
                else target_freq = 80;

                // Restrict frequency within local NVRAM limits
                if (target_freq > (uint32_t)max_bios_freq) {
                    target_freq = (uint32_t)max_bios_freq;
                }
            }

            // Apply PMU frequency changes exclusively upon threshold transitions
            if (target_freq != current_applied_freq) {
                esp_pm_config_t pm_cfg = {
                    .max_freq_mhz = target_freq,
                    .min_freq_mhz = target_freq,
                    .light_sleep_enable = false
                };
                esp_pm_configure(&pm_cfg);
                current_applied_freq = target_freq;

                // === LOG CPU FREQUENCY MODULATIONS ===
                ESP_LOGI(TAG, "Load: %lu%% | Temp: %lu C | New Freq: %lu MHz", load, ulp_me_temperature, target_freq);
            }
        }

        last_total_time = now;
        last_idle_time = idle_now;

        // Governor recalculation frequency rate set to 100ms
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ─── GOVERNOR SUBSYSTEM INITIALIZER ──────────────────────────────────────────
void power_governor_start(void) {
    // 1. Temperature monitoring task must run globally across all modes
    if (temp_task_handle == NULL) {
        xTaskCreate(openc6_temperature_task, "tsens", 2048, NULL, 5, &temp_task_handle);
    }

    // =========================================================================
    // 2. BROADCAST SYSTEM CRITICAL LIMITS TO ME SHARED MEMORY
    // =========================================================================
    if (is_me_enabled) {
        cpu_freq_t max_bios_freq;
        nvram_get_cpu_freq(&max_bios_freq);

        uint8_t temp_th = 55;
        uint8_t temp_em = 75;
        nvram_get_thermal_limits(&temp_th, &temp_em);

        ulp_me_max_freq       = (uint32_t)max_bios_freq;
        ulp_me_throttle_temp  = (uint32_t)temp_th;
        ulp_me_emergency_temp = (uint32_t)temp_em;
    }

    // 3. Spawn SchedUtil governor loop only if DYNAMIC mode is configured in BIOS
    cpu_governor_t gov;
    nvram_get_cpu_governor(&gov);

    if (gov == GOV_DYNAMIC && gov_task_handle == NULL) {
        xTaskCreate(openc6_schedutil_task, "schedutil", 3072, NULL, configMAX_PRIORITIES - 1, &gov_task_handle);
        ESP_LOGI(TAG, "OpenC6 Dynamic Governor active (Delegated to ME).");
    }
}
