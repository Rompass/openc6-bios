#include "esp_log.h"
#include "nvs_flash.h"
#include "nvram.h"
#include "nvram_schema.h"

static const char *TAG = "NVRAM_DEF";

/**
 * @brief Performs a factory reset on BIOS settings (Load Setup Defaults / Clear CMOS).
 * Wipes the active NVS partition completely and provisions safe default parameters for all subsystems.
 *
 * @return ESP_OK on success, or an error code from the underlying flash storage layer.
 */
esp_err_t nvram_load_defaults(void)
{
    ESP_LOGW(TAG, "Initiating BIOS Factory Reset (Clear CMOS)...");

    /* 1. Wipe the NVS partition completely to purge corrupt or obsolete namespace keys */
    esp_err_t err = nvs_flash_erase();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase NVS partition: %s", esp_err_to_name(err));
        return err;
    }

    /* 2. Re-initialize NVS partition structures following flash erasure */
    err = nvram_init();
    if (err != ESP_OK) {
        return err;
    }

    /* 3. Base platform power and boot diagnostic settings */
    ESP_LOGI(TAG, "Setting Default: DC_LOSS_ACTION = POWER_OFF");
    nvram_set_dc_loss_action(DC_LOSS_POWER_OFF);

    ESP_LOGI(TAG, "Setting Default: POST_LED_MODE = ENABLED");
    nvram_set_post_led_mode(POST_LED_ENABLED);

    /* 4. Default Aura Sync RGB parameters */
    ESP_LOGI(TAG, "Setting Default: AURA_MODE = RAINBOW");
    nvram_set_aura_mode(AURA_RAINBOW);

    ESP_LOGI(TAG, "Setting Default: AURA_BRIGHTNESS = 128 (50%%)");
    nvram_set_aura_brightness(128);

    /* 5. AI Tweaker: CPU clock and power governor profiles */
    ESP_LOGI(TAG, "Setting Default: CPU_FREQ = 160 MHz (Turbo)");
    nvram_set_cpu_freq(CPU_FREQ_160MHZ);

    /* Dynamic governor scales core frequency based on FreeRTOS run-time load */
    ESP_LOGI(TAG, "Setting Default: CPU_GOVERNOR = DYNAMIC");
    nvram_set_cpu_governor(GOV_DYNAMIC);

    /* Brownout detector triggers chip reset below 2.8V to avoid SRAM corruption */
    ESP_LOGI(TAG, "Setting Default: BOD_LEVEL = STRICT (2.8V)");
    nvram_set_bod_level(BOD_STRICT);

    /* Thermal limits: throttle down at 55 deg C, emergency soft-off at 75 deg C */
    ESP_LOGI(TAG, "Setting Default: Thermal Limits (Throttle=55C, Emergency=75C)");
    nvram_set_thermal_limits(VAL_TEMP_THROTTLE_DEFAULT, VAL_TEMP_EMERGENCY_DEFAULT);

    /* 6. Purge Wi-Fi station network credentials */
    ESP_LOGI(TAG, "Setting Default: Clearing WiFi credentials");
    nvram_set_wifi_sta_config("", "");

    /* Fallback default PXE boot endpoint */
    ESP_LOGI(TAG, "Setting Default: PXE_URL = %s", VAL_PXE_URL_DEFAULT);
    nvram_set_pxe_url(VAL_PXE_URL_DEFAULT);

    /* Reset pending wireless BIOS OTA firmware update state */
    ESP_LOGI(TAG, "Setting Default: BIOS_UPDATE_STATE = NONE");
    nvram_set_bios_update_state(BIOS_UPDATE_NONE);

    /* 7. Remote Web Shell (c6wsh): Disabled by default to keep RF baseband unpowered
     * and reserve ~90 KB SRAM for bare-metal U-mode payload memory arenas */
    ESP_LOGI(TAG, "Setting Default: C6WSH_STATE = DISABLED");
    nvram_set_c6wsh_state(C6WSH_DISABLED);

    /* 8. ZC6 Compression and ZSWAP Engine Mode */
    ESP_LOGI(TAG, "Setting Default: ZC6_SWAP_MODE = BOTH (Flash + RAM ZSWAP)");
    nvram_set_zc6_swap_mode(VAL_ZC6_SWAP_DEFAULT);

    ESP_LOGI(TAG, "Setting Default: WIFI_COUNTRY = %s", VAL_WIFI_COUNTRY_DEFAULT);
    nvram_set_wifi_country(VAL_WIFI_COUNTRY_DEFAULT);

    ESP_LOGI(TAG, "BIOS Setup Defaults loaded successfully.");
    return ESP_OK;
}
