#include "management_engine.h"
#include "me_shared.h"
#include "hal_board.h"
#include "esp_log.h"

static const char *TAG = "ME_HOST";
static bool me_active = false;

#if HAL_CAP_LP_CORE

#include "ulp_lp_core.h"
#include "me_core.h"

extern const uint8_t me_core_bin_start[] asm("_binary_me_core_bin_start");
extern const uint8_t me_core_bin_end[]   asm("_binary_me_core_bin_end");

static bool is_valid_boot_reason(uint32_t r) {
    return r == ME_BOOT_REASON_NONE        ||
    r == ME_BOOT_REASON_NORMAL      ||
    r == ME_BOOT_REASON_SETUP       ||
    r == ME_BOOT_REASON_FORCE_RESET ||
    r == ME_BOOT_REASON_WDT         ||
    r == ME_BOOT_REASON_THERMAL;
}

void management_engine_init(bool is_enabled, bool is_cold_boot) {
    if (!is_enabled) {
        me_active = false;
        ESP_LOGW(TAG, "Management Engine is DISABLED. Running without LP-Core, Thermal Guard & SchedUtil.");
        return;
    }

    if (!is_cold_boot) {
        ESP_LOGI(TAG, "ME already running (wakeup path). Skipping LP-Core re-deploy.");
        me_active = true;
        return;
    }

    ESP_LOGI(TAG, "Deploying OpenC6 Management Engine to LP-Core...");

    esp_err_t err = ulp_lp_core_load_binary(me_core_bin_start,
                                            (me_core_bin_end - me_core_bin_start));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load ME binary: %s", esp_err_to_name(err));
        me_active = false;
        return;
    }

    ulp_me_is_enabled  = 1;
    ulp_me_boot_reason = ME_BOOT_REASON_NONE;

    ulp_lp_core_cfg_t cfg = { .wakeup_source = ULP_LP_CORE_WAKEUP_SOURCE_HP_CPU };
    ESP_ERROR_CHECK(ulp_lp_core_run(&cfg));

    me_active = true;
    ESP_LOGI(TAG, "ME is Active. Hardware WDT & Button logic delegated to LP-Core.");
}

uint32_t management_engine_get_boot_reason(void) {
    uint32_t r = ulp_me_boot_reason;
    if (!is_valid_boot_reason(r)) {
        ESP_LOGW(TAG, "LP RAM has garbage boot reason (0x%08lX) — treating as NONE", r);
        return ME_BOOT_REASON_NONE;
    }
    return r;
}

void management_engine_clear_boot_reason(void) {
    ulp_me_boot_reason = ME_BOOT_REASON_NONE;
}

void management_engine_pet_watchdog(void) {
    if (!me_active) return;
    ulp_me_wdt_counter++;
}

#else /* HAL_CAP_LP_CORE == 0 (Target without LP-Core hardware) */

/* Fallback shared symbols so shell/web_ui link cleanly without ULP binary */
uint32_t ulp_me_hp_is_awake   = 0;
uint32_t ulp_me_boot_reason   = ME_BOOT_REASON_NONE;
uint32_t ulp_me_wdt_counter   = 0;
uint32_t ulp_me_hp_busy_ticks = 0;
uint32_t ulp_me_hp_idle_ticks = 0;
uint32_t ulp_me_cpu_load      = 0;
uint32_t ulp_me_max_freq      = 0;
uint32_t ulp_me_min_freq      = 0;
uint32_t ulp_me_target_freq   = 0;
uint32_t ulp_me_temperature   = 0;
uint32_t ulp_me_throttle_temp = 0;
uint32_t ulp_me_emergency_temp= 0;

void management_engine_init(bool is_enabled, bool is_cold_boot) {
    (void)is_enabled;
    (void)is_cold_boot;
    me_active = false;
    ESP_LOGW(TAG, "No hardware LP-Core on this target. ME, Thermal Guard & SchedUtil disabled.");
}

uint32_t management_engine_get_boot_reason(void) {
    return ME_BOOT_REASON_NONE;
}

void management_engine_clear_boot_reason(void) {}
void management_engine_pet_watchdog(void) {}

#endif /* HAL_CAP_LP_CORE */

bool management_engine_is_active(void) {
    return me_active;
}

bool management_engine_check_force_shutdown(void) {
    return false;
}
