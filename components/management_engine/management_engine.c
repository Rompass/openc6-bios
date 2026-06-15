#include "management_engine.h"
#include "me_shared.h"
#include "ulp_lp_core.h"
#include "me_core.h"
#include "esp_log.h"
#include "esp_system.h"

extern const uint8_t me_core_bin_start[] asm("_binary_me_core_bin_start");
extern const uint8_t me_core_bin_end[]   asm("_binary_me_core_bin_end");

static const char *TAG = "ME_HOST";
static bool me_active = false;

// Verify the value matches a known bootsource enum.
// Uninitialized LP RAM contains junk on cold boots; validation is required
// to prevent random RAM garbage from being processed as a valid boot signal.
static bool is_valid_boot_reason(uint32_t r) {
    return r == ME_BOOT_REASON_NONE        ||
    r == ME_BOOT_REASON_NORMAL      ||
    r == ME_BOOT_REASON_SETUP       ||
    r == ME_BOOT_REASON_FORCE_RESET ||
    r == ME_BOOT_REASON_WDT         || // Diagnostic expansion
    r == ME_BOOT_REASON_THERMAL;       // Diagnostic expansion
}

void management_engine_init(bool is_enabled, bool is_cold_boot) {
    if (!is_enabled) {
        ESP_LOGW(TAG, "Management Engine is DISABLED. System running in Legacy Mode.");
        return;
    }

    if (!is_cold_boot) {
        // Warm wakeup path: LP core is already running, LP RAM is intact. Skip re-deployment.
        ESP_LOGI(TAG, "ME already running (wakeup path). Skipping LP-Core re-deploy.");
        me_active = true;
        return;
    }

    ESP_LOGI(TAG, "Deploying OpenC6 Management Engine to LP-Core...");

    esp_err_t err = ulp_lp_core_load_binary(me_core_bin_start,
                                            (me_core_bin_end - me_core_bin_start));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load ME binary: %s", esp_err_to_name(err));
        return;
    }

    // Initialize LP RAM parameters strictly after binary deployment.
    // Note: get_boot_reason() has already fetched values prior to this step.
    ulp_me_is_enabled  = 1;
    ulp_me_boot_reason = ME_BOOT_REASON_NONE;

    ulp_lp_core_cfg_t cfg = { .wakeup_source = ULP_LP_CORE_WAKEUP_SOURCE_HP_CPU };
    ESP_ERROR_CHECK(ulp_lp_core_run(&cfg));

    me_active = true;
    ESP_LOGI(TAG, "ME is Active. Hardware WDT & Button logic delegated to LP-Core.");
}

bool management_engine_check_force_shutdown(void) {
    return false; // Watchdog reset is processed via me_reason inside bios_core_start
}

// MUST be called before management_engine_init().
// On cold boots, LP RAM is uninitialized. Default to NONE if garbage is detected.
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

// HP Core triggers this every ~2s to announce stable kernel execution.
// LP Core enforces a 15-second timeout (ME_WDT_TIMEOUT_MS) before hardware reset.
void management_engine_pet_watchdog(void) {
    if (!me_active) return;
    ulp_me_wdt_counter++;
}
