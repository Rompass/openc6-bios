#include "esp_log.h"
#include "nvs_flash.h"
#include "nvram.h"
#include "nvram_schema.h"

static const char *TAG = "NVRAM_DEF";

/**
 * @brief Performs a factory reset on BIOS settings (Load Setup Defaults).
 * Wipes the active NVS partition completely and provisions safe default parameters for all subsystems.
 */
esp_err_t nvram_load_defaults(void)
{
    ESP_LOGW(TAG, "Initiating BIOS Factory Reset (Clear CMOS)...");

    // 1. Wipe the NVS partition completely (deletes all namespaces and key-value entries)
    esp_err_t err = nvs_flash_erase();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase NVS partition: %s", esp_err_to_name(err));
        return err;
    }

    // 2. Re-initialize NVS partition space after erasure
    err = nvram_init();
    if (err != ESP_OK) {
        return err;
    }

    // 3. Write default values for primary system configurations
    ESP_LOGI(TAG, "Setting Default: DC_LOSS_ACTION = POWER_OFF");
    nvram_set_dc_loss_action(DC_LOSS_POWER_OFF);

    ESP_LOGI(TAG, "Setting Default: POST_LED_MODE = ENABLED");
    nvram_set_post_led_mode(POST_LED_ENABLED);

    // 4. Write default configurations for Aura Sync RGB
    ESP_LOGI(TAG, "Setting Default: AURA_MODE = RAINBOW");
    nvram_set_aura_mode(AURA_RAINBOW);

    ESP_LOGI(TAG, "Setting Default: AURA_BRIGHTNESS = 128 (50%%)");
    nvram_set_aura_brightness(128);

    // 5. AI TWEAKER: CPU Frequency & Power profiles configuration
    ESP_LOGI(TAG, "Setting Default: CPU_FREQ = 160 MHz (Turbo)");
    nvram_set_cpu_freq(CPU_FREQ_160MHZ);

    // Set CPU Governor mode to DYNAMIC by default for SchedUtil load scaling support
    ESP_LOGI(TAG, "Setting Default: CPU_GOVERNOR = DYNAMIC");
    nvram_set_cpu_governor(GOV_DYNAMIC);

    ESP_LOGI(TAG, "Setting Default: BOD_LEVEL = STRICT (2.8V)");
    nvram_set_bod_level(BOD_STRICT);

    // Write thermal throttling and safety limit defaults
    ESP_LOGI(TAG, "Setting Default: Thermal Limits (Throttle=55C, Emergency=75C)");
    nvram_set_thermal_limits(VAL_TEMP_THROTTLE_DEFAULT, VAL_TEMP_EMERGENCY_DEFAULT);

    // 6. Purge existing station Wi-Fi network credentials
    ESP_LOGI(TAG, "Setting Default: Clearing WiFi credentials");
    nvram_set_wifi_sta_config("", "");

    // Commit default PXE target server path URL
    ESP_LOGI(TAG, "Setting Default: PXE_URL = %s", VAL_PXE_URL_DEFAULT);
    nvram_set_pxe_url(VAL_PXE_URL_DEFAULT);

    // Reset pending wireless BIOS OTA firmware update state flag to NONE
    ESP_LOGI(TAG, "Setting Default: BIOS_UPDATE_STATE = NONE");
    nvram_set_bios_update_state(BIOS_UPDATE_NONE);

    // 7. Management Engine (ME) baseline state
    ESP_LOGI(TAG, "Setting Default: ME_STATE = ENABLED");
    nvram_set_me_state(ME_ENABLED);

    ESP_LOGI(TAG, "BIOS Setup Defaults loaded successfully.");

    return ESP_OK;
}
