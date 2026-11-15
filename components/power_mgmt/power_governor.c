#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_freertos_hooks.h"
#include "hal_board.h"
#include "hal_clock.h"
#include "hal_thermal.h"
#include "management_engine.h"
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

static void openc6_temperature_task(void *arg)
{
    bool tsens_ready = hal_thermal_init();

    while (1) {
        if (tsens_ready) {
            float tsens_value = 0.0f;
            if (hal_thermal_read_celsius(&tsens_value)) {
                ulp_me_temperature = (uint32_t)tsens_value;
            }
        } else {
            ulp_me_temperature = 40;
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

static void openc6_pm_sync_task(void *arg)
{
    while (1) {
        uint32_t target_freq = ulp_me_target_freq;

        if (target_freq > 0) {
            /* 1. Read actual current hardware clock state directly from silicon */
            uint32_t current_hw_freq = hal_clock_get_cpu_mhz();

            /* 2. If physical hardware clock diverged from governor target, enforce it immediately */
            if (current_hw_freq != target_freq) {
                hal_clock_set_cpu_mhz(target_freq);

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

void power_governor_start(void)
{
    if (!HAL_CAP_LP_CORE || !is_me_enabled || !management_engine_is_active()) {
        ESP_LOGW(TAG, "SchedUtil & Thermal Guard skipped (LP-Core ME disabled/unavailable).");
        return;
    }

    /* Mute verbose ESP-IDF PM framework logs across all clock transitions */
    esp_log_level_set("pm", ESP_LOG_WARN);

    /* 1. Junction temperature telemetry */
    #if HAL_CAP_THERMAL_SENSOR
    if (temp_task_handle == NULL) {
        xTaskCreate(openc6_temperature_task, "tsens", 2048, NULL, 5, &temp_task_handle);
    }
    #endif

    /* 2. Broadcast baseline CMOS thresholds and bounds to LP-RAM */
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
