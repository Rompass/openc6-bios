#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "nvram.h"

static const char *TAG = "NVRAM";

esp_err_t nvram_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

/**
 * @brief Restores factory system defaults (Clear CMOS behavior).
 */
esp_err_t nvram_load_defaults(void) {
    ESP_LOGW(TAG, "Restoring factory defaults (Clear CMOS)...");

    nvs_flash_erase();
    nvram_init();

    // 1. Primary System Parameters
    nvram_set_dc_loss_action(DC_LOSS_POWER_OFF);
    nvram_set_post_led_mode(POST_LED_ENABLED);

    // 2. Aura Sync RGB Parameters
    nvram_set_aura_mode(AURA_RAINBOW);
    nvram_set_aura_brightness(128);

    // 3. AI Tweaker (CPU & Power Core Tuning) - Apply safe defaults
    nvram_set_cpu_freq(CPU_FREQ_160MHZ);      // Standard maximum
    nvram_set_cpu_governor(GOV_PERFORMANCE);  // Solid power profile
    nvram_set_bod_level(BOD_STRICT);          // Strict Brownout Detector threshold protection

    // 3.1. Management Engine status and thermal thresholds
    nvram_set_me_state(ME_ENABLED);
    nvram_set_thermal_limits(VAL_TEMP_THROTTLE_DEFAULT, VAL_TEMP_EMERGENCY_DEFAULT);

    // 4. Wipe Wi-Fi network credentials and write the fallback default PXE Boot target URL
    nvram_set_wifi_sta_config("", "");
    nvram_set_pxe_url(VAL_PXE_URL_DEFAULT);

    ESP_LOGI(TAG, "Factory defaults loaded successfully.");
    return ESP_OK;
}

// --- Basic System Parameters (DC Loss Action, Post LED Diagnostics) ---
esp_err_t nvram_get_dc_loss_action(dc_loss_action_t *action) {
    nvs_handle_t handle;
    uint8_t val = DC_LOSS_POWER_OFF;
    esp_err_t err = nvs_open(BIOS_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        nvs_get_u8(handle, "dc_loss", &val);
        nvs_close(handle);
    }
    *action = (dc_loss_action_t)val;
    return err;
}

esp_err_t nvram_set_dc_loss_action(dc_loss_action_t action) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(BIOS_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    nvs_set_u8(handle, "dc_loss", (uint8_t)action);
    nvs_commit(handle);
    nvs_close(handle);
    return ESP_OK;
}

esp_err_t nvram_get_post_led_mode(post_led_mode_t *mode) {
    nvs_handle_t handle;
    uint8_t val = POST_LED_ENABLED;
    esp_err_t err = nvs_open(BIOS_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        nvs_get_u8(handle, "post_led", &val);
        nvs_close(handle);
    }
    *mode = (post_led_mode_t)val;
    return err;
}

esp_err_t nvram_set_post_led_mode(post_led_mode_t mode) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(BIOS_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    nvs_set_u8(handle, "post_led", (uint8_t)mode);
    nvs_commit(handle);
    nvs_close(handle);
    return ESP_OK;
}

// --- Wi-Fi Station Configuration ---
esp_err_t nvram_get_wifi_sta_config(char* ssid, size_t ssid_len, char* pass, size_t pass_len) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(BIOS_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    if (ssid) {
        err = nvs_get_str(handle, "wifi_ssid", ssid, &ssid_len);
        if (err != ESP_OK) ssid[0] = '\0';
    }
    if (pass) {
        err = nvs_get_str(handle, "wifi_pass", pass, &pass_len);
        if (err != ESP_OK) pass[0] = '\0';
    }
    nvs_close(handle);
    return ESP_OK;
}

esp_err_t nvram_set_wifi_sta_config(const char* ssid, const char* pass) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(BIOS_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    nvs_set_str(handle, "wifi_ssid", ssid);
    nvs_set_str(handle, "wifi_pass", pass);
    nvs_commit(handle);
    nvs_close(handle);
    return ESP_OK;
}

// --- Wireless PXE Configuration Interfaces ---
esp_err_t nvram_get_pxe_url(char* url, size_t max_len) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(BIOS_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        // Fall back to compiling and returning the secure default string constant if NVS space is clean
        strncpy(url, VAL_PXE_URL_DEFAULT, max_len - 1);
        url[max_len - 1] = '\0';
        return ESP_OK;
    }
    err = nvs_get_str(handle, "pxe_url", url, &max_len);
    if (err != ESP_OK) {
        strncpy(url, VAL_PXE_URL_DEFAULT, max_len - 1);
        url[max_len - 1] = '\0';
    }
    nvs_close(handle);
    return ESP_OK;
}

esp_err_t nvram_set_pxe_url(const char* url) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(BIOS_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_str(handle, "pxe_url", url);
    if (err == ESP_OK) {
        nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

// --- Aura Sync RGB Configuration ---
esp_err_t nvram_get_aura_mode(aura_mode_t *mode) {
    nvs_handle_t handle;
    uint8_t val = AURA_RAINBOW;
    esp_err_t err = nvs_open(BIOS_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        nvs_get_u8(handle, "aura_mode", &val);
        nvs_close(handle);
    }
    *mode = (aura_mode_t)val;
    return err;
}

esp_err_t nvram_set_aura_mode(aura_mode_t mode) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(BIOS_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    nvs_set_u8(handle, "aura_mode", (uint8_t)mode);
    nvs_commit(handle);
    nvs_close(handle);
    return ESP_OK;
}

esp_err_t nvram_get_aura_brightness(uint8_t *brightness) {
    nvs_handle_t handle;
    uint8_t val = 128;
    esp_err_t err = nvs_open(BIOS_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        nvs_get_u8(handle, "aura_br", &val);
        nvs_close(handle);
    }
    *brightness = val;
    return err;
}

esp_err_t nvram_set_aura_brightness(uint8_t brightness) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(BIOS_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    nvs_set_u8(handle, "aura_br", brightness);
    nvs_commit(handle);
    nvs_close(handle);
    return ESP_OK;
}

// --- AI TWEAKER: CPU Frequency Limits ---
esp_err_t nvram_get_cpu_freq(cpu_freq_t *freq) {
    nvs_handle_t handle;
    uint8_t val = CPU_FREQ_160MHZ;
    esp_err_t err = nvs_open(BIOS_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        nvs_get_u8(handle, "cpu_freq", &val);
        nvs_close(handle);
    }
    *freq = (cpu_freq_t)val;
    return err;
}

esp_err_t nvram_set_cpu_freq(cpu_freq_t freq) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(BIOS_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    nvs_set_u8(handle, "cpu_freq", (uint8_t)freq);
    nvs_commit(handle);
    nvs_close(handle);
    return ESP_OK;
}

// --- AI TWEAKER: CPU SchedUtil Governor Settings ---
esp_err_t nvram_get_cpu_governor(cpu_governor_t *gov) {
    nvs_handle_t handle;
    uint8_t val = GOV_PERFORMANCE;
    esp_err_t err = nvs_open(BIOS_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        nvs_get_u8(handle, "cpu_gov", &val);
        nvs_close(handle);
    }
    *gov = (cpu_governor_t)val;
    return err;
}

esp_err_t nvram_set_cpu_governor(cpu_governor_t gov) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(BIOS_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    nvs_set_u8(handle, "cpu_gov", (uint8_t)gov);
    nvs_commit(handle);
    nvs_close(handle);
    return ESP_OK;
}

// --- AI TWEAKER: Brownout Detector (BOD) Protection Levels ---
esp_err_t nvram_get_bod_level(bod_level_t *level) {
    nvs_handle_t handle;
    uint8_t val = BOD_STRICT;
    esp_err_t err = nvs_open(BIOS_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        nvs_get_u8(handle, "bod_lvl", &val);
        nvs_close(handle);
    }
    *level = (bod_level_t)val;
    return err;
}

esp_err_t nvram_set_bod_level(bod_level_t level) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(BIOS_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    nvs_set_u8(handle, "bod_lvl", (uint8_t)level);
    nvs_commit(handle);
    nvs_close(handle);
    return ESP_OK;
}

// --- Management Engine (ME) Operations ---
esp_err_t nvram_get_me_state(me_state_t *state) {
    nvs_handle_t handle;
    uint8_t val = ME_ENABLED;
    esp_err_t err = nvs_open(BIOS_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        nvs_get_u8(handle, "me_state", &val);
        nvs_close(handle);
    }
    *state = (me_state_t)val;
    return err;
}

esp_err_t nvram_set_me_state(me_state_t state) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(BIOS_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    nvs_set_u8(handle, "me_state", (uint8_t)state);
    nvs_commit(handle);
    nvs_close(handle);
    return ESP_OK;
}

// --- AI TWEAKER: Thermal Limits Management ---
esp_err_t nvram_get_thermal_limits(uint8_t *throttle, uint8_t *emergency) {
    nvs_handle_t handle;
    uint8_t th = VAL_TEMP_THROTTLE_DEFAULT;
    uint8_t em = VAL_TEMP_EMERGENCY_DEFAULT;
    esp_err_t err = nvs_open(BIOS_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        nvs_get_u8(handle, "temp_th", &th);
        nvs_get_u8(handle, "temp_em", &em);
        nvs_close(handle);
    }
    *throttle = th;
    *emergency = em;
    return err;
}

esp_err_t nvram_set_thermal_limits(uint8_t throttle, uint8_t emergency) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(BIOS_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    nvs_set_u8(handle, "temp_th", throttle);
    nvs_set_u8(handle, "temp_em", emergency);
    nvs_commit(handle);
    nvs_close(handle);
    return ESP_OK;
}

// --- Over-The-Air (OTA) System Flag Config ---
esp_err_t nvram_get_bios_update_state(bios_update_state_t *state) {
    nvs_handle_t handle;
    uint8_t val = BIOS_UPDATE_NONE;
    esp_err_t err = nvs_open(BIOS_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        nvs_get_u8(handle, "ota_pend", &val);
        nvs_close(handle);
    }
    *state = (bios_update_state_t)val;
    return err;
}

esp_err_t nvram_set_bios_update_state(bios_update_state_t state) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(BIOS_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    nvs_set_u8(handle, "ota_pend", (uint8_t)state);
    nvs_commit(handle);
    nvs_close(handle);
    return ESP_OK;
}
