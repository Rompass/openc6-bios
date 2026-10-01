#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_pm.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_freertos_hooks.h"
#include "driver/temperature_sensor.h"
#include "rom/ets_sys.h"
#include "nvram.h"
#include "me_shared.h"

/* Telemetry verbosity toggle: 1 = enable live frequency transition logging, 0 = silent production */
#define SCHEDUTIL_DEBUG 0

static const char *TAG = "SCHEDUTIL";
static TaskHandle_t sync_task_handle = NULL;
static TaskHandle_t temp_task_handle = NULL;

extern bool is_me_enabled;

/* FreeRTOS internal task tracking table residing in internal DRAM */
extern void * volatile pxCurrentTCBs[];

/**
 * @brief FreeRTOS SysTick ISR hook executing Linux-style jiffies sampling.
 * Directly increments busy or idle tick accumulators based on active task context.
 * Reads pxCurrentTCBs[0] directly from internal DRAM to strictly avoid calling
 * Flash-resident APIs while Flash cache is disabled during sector erase operations.
 */
static TaskHandle_t s_idle_task_handle = NULL;

static void IRAM_ATTR openc6_pm_tick_hook(void)
{
    /* Safe DRAM dereference: bypasses Flash-resident xTaskGetCurrentTaskHandle */
    if (s_idle_task_handle != NULL && (TaskHandle_t)pxCurrentTCBs[0] == s_idle_task_handle) {
        ulp_me_hp_idle_ticks++;
    } else {
        ulp_me_hp_busy_ticks++;
    }
}

/**
 * @brief Background temperature sampling task.
 * Periodically reads on-die silicon sensor and posts calibrated Celsius metric to LP-RAM.
 */
static void openc6_temperature_task(void *arg)
{
    temperature_sensor_handle_t temp_sensor = NULL;

    temperature_sensor_config_t temp_cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100);
    esp_err_t err = temperature_sensor_install(&temp_cfg, &temp_sensor);

    if (err != ESP_OK) {
        temp_cfg.range_min = -10;
        temp_cfg.range_max = 80;
        err = temperature_sensor_install(&temp_cfg, &temp_sensor);
    }

    if (err == ESP_OK) {
        temperature_sensor_enable(temp_sensor);
        ESP_LOGI(TAG, "Hardware Temperature Sensor initialized successfully!");
    } else {
        ESP_LOGE(TAG, "Failed to init TSENS: %s", esp_err_to_name(err));
    }

    while (1) {
        if (temp_sensor != NULL) {
            float tsens_value = 0.0f;
            esp_err_t res = temperature_sensor_get_celsius(temp_sensor, &tsens_value);
            if (res == ESP_OK) {
                ulp_me_temperature = (uint32_t)tsens_value;
            }
        } else {
            ulp_me_temperature = 40;
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

/**
 * @brief Hardware clock actuator task.
 * Constantly verifies actual PCR register state against governor target.
 * Immediately restores target frequency if internal IDF drivers (Wi-Fi/TRNG) reset the clock.
 */
static void openc6_pm_sync_task(void *arg)
{
    while (1) {
        uint32_t target_freq = ulp_me_target_freq;

        if (target_freq > 0) {
            /* 1. Read actual current hardware clock state directly from silicon */
            uint32_t pcr = *((volatile uint32_t *)0x60096118UL);
            uint32_t current_hw_freq = 160;

            if (pcr & (1UL << 16)) {
                current_hw_freq = 120;
            } else if (((pcr >> 8) & 0xFF) == 1) {
                current_hw_freq = 80;
            }

            /* 2. If physical hardware clock diverged from governor target, enforce it immediately */
            if (current_hw_freq != target_freq) {
                uint32_t reg = pcr & ~0x1FF00UL;

                if (target_freq >= 160) {
                    /* 160 MHz: div_num = 0, force_120m = 0 */
                } else if (target_freq >= 120) {
                    /* 120 MHz: force_120m = 1 */
                    reg |= (1UL << 16);
                } else {
                    /* 80 MHz: div_num = 1 (bits [15:8] = 1) */
                    reg |= (1UL << 8);
                }

                *((volatile uint32_t *)0x60096118UL) = reg;
                ets_update_cpu_frequency(target_freq);

                #if SCHEDUTIL_DEBUG
                ESP_LOGI(TAG, "Hardware Clock Enforced: %lu MHz (Load: %lu%%, Temp: %lu C)",
                         (unsigned long)target_freq,
                         (unsigned long)ulp_me_cpu_load,
                         (unsigned long)ulp_me_temperature);
                #endif
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/**
 * @brief Initializes thermal sensor telemetry and activates hardware clock regulation.
 * Caches the FreeRTOS Idle task handle in internal SRAM once at startup to avoid
 * dereferencing Flash-resident APIs from the SysTick ISR.
 */
void power_governor_start(void)
{
    /* Mute verbose ESP-IDF PM framework logs across all clock transitions */
    esp_log_level_set("pm", ESP_LOG_WARN);

    /* 1. Junction temperature telemetry */
    if (temp_task_handle == NULL) {
        xTaskCreate(openc6_temperature_task, "tsens", 2048, NULL, 5, &temp_task_handle);
    }

    /* 2. Broadcast baseline CMOS thresholds and bounds to LP-RAM */
    if (is_me_enabled) {
        cpu_freq_t bios_freq;
        cpu_governor_t gov;
        nvram_get_cpu_freq(&bios_freq);
        nvram_get_cpu_governor(&gov);

        uint8_t temp_th = 55;
        uint8_t temp_em = 75;
        nvram_get_thermal_limits(&temp_th, &temp_em);

        ulp_me_throttle_temp  = (uint32_t)temp_th;
        ulp_me_emergency_temp = (uint32_t)temp_em;

        if (gov == GOV_DYNAMIC) {
            ulp_me_min_freq = 80;
            ulp_me_max_freq = (uint32_t)bios_freq;
        } else {
            /* Fixed Performance Mode: clamp min and max to the exact setpoint */
            ulp_me_min_freq = (uint32_t)bios_freq;
            ulp_me_max_freq = (uint32_t)bios_freq;
        }
    }

    /* Cache Idle task handle in DRAM once to prevent Flash memory access during SysTick ISR */
    s_idle_task_handle = xTaskGetIdleTaskHandle();

    /* 3. Register zero-overhead FreeRTOS SysTick hook for autonomous load measurement */
    esp_register_freertos_tick_hook(openc6_pm_tick_hook);

    /* 4. Spawn hardware clock actuator task (enforces setpoint in both Fixed and Dynamic modes) */
    if (sync_task_handle == NULL) {
        xTaskCreate(openc6_pm_sync_task, "pm_sync", 1536, NULL, 1, &sync_task_handle);
        ESP_LOGI(TAG, "Hardware Clock Actuator active (Delegated to LP-Core).");
    }
}
