#pragma once

#include "esp_err.h"
#include "nvram_schema.h"

esp_err_t nvram_init(void);
esp_err_t nvram_load_defaults(void);

esp_err_t nvram_get_dc_loss_action(dc_loss_action_t *action);
esp_err_t nvram_set_dc_loss_action(dc_loss_action_t action);

esp_err_t nvram_get_post_led_mode(post_led_mode_t *mode);
esp_err_t nvram_set_post_led_mode(post_led_mode_t mode);

esp_err_t nvram_get_wifi_sta_config(char* ssid, size_t ssid_len, char* pass, size_t pass_len);
esp_err_t nvram_set_wifi_sta_config(const char* ssid, const char* pass);

/**
 * @brief Retrieves the active Wi-Fi regulatory domain ISO 3166-1 alpha-2 country code.
 *
 * @param country Buffer to store the 2-letter country string (min 3 bytes).
 * @param max_len Size of destination buffer.
 * @return ESP_OK on success, or fallback default if key is not provisioned.
 */
esp_err_t nvram_get_wifi_country(char* country, size_t max_len);

/**
 * @brief Sets the active Wi-Fi regulatory domain ISO 3166-1 alpha-2 country code.
 *
 * @param country Null-terminated 2-letter country code string (e.g. "UA", "US", "JP", "01").
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t nvram_set_wifi_country(const char* country);

esp_err_t nvram_get_pxe_url(char* url, size_t max_len);
esp_err_t nvram_set_pxe_url(const char* url);

esp_err_t nvram_get_aura_mode(aura_mode_t *mode);
esp_err_t nvram_set_aura_mode(aura_mode_t mode);

esp_err_t nvram_get_aura_brightness(uint8_t *brightness);
esp_err_t nvram_set_aura_brightness(uint8_t brightness);

/**
 * @brief Retrieves the active ZC6 compression and ZSWAP operating mode.
 *
 * @param mode Destination pointer to store the retrieved zc6_swap_mode_t.
 * @return ESP_OK on success, or NVS error code if flash read fails.
 */
esp_err_t nvram_get_zc6_swap_mode(zc6_swap_mode_t *mode);

/**
 * @brief Sets and commits the ZC6 compression and ZSWAP mode in CMOS NVRAM.
 *
 * @param mode Target mode (DISABLED, FLASH, RAM, or BOTH).
 * @return ESP_OK on success, or error code on flash commit failure.
 */
esp_err_t nvram_set_zc6_swap_mode(zc6_swap_mode_t mode);

esp_err_t nvram_get_cpu_freq(cpu_freq_t *freq);
esp_err_t nvram_set_cpu_freq(cpu_freq_t freq);

esp_err_t nvram_get_cpu_governor(cpu_governor_t *gov);
esp_err_t nvram_set_cpu_governor(cpu_governor_t gov);

esp_err_t nvram_get_bod_level(bod_level_t *level);
esp_err_t nvram_set_bod_level(bod_level_t level);

esp_err_t nvram_get_thermal_limits(uint8_t *throttle, uint8_t *emergency);
esp_err_t nvram_set_thermal_limits(uint8_t throttle, uint8_t emergency);

esp_err_t nvram_get_bios_update_state(bios_update_state_t *state);
esp_err_t nvram_set_bios_update_state(bios_update_state_t state);

/**
 * @brief Retrieves the Web Shell (c6wsh) activation state from CMOS NVRAM.
 * Reads the persistent toggle to determine whether the BIOS should establish
 * station L3 connectivity at POST and launch the c6wsh remote terminal service.
 *
 * @param state Destination pointer to store the retrieved c6wsh_state_t value.
 * @return ESP_OK on success, or an NVS error code if flash read fails.
 */
esp_err_t nvram_get_c6wsh_state(c6wsh_state_t *state);

/**
 * @brief Sets and commits the Web Shell (c6wsh) activation state in CMOS NVRAM.
 * Commits the configuration into flash memory, ensuring persistence across reboots.
 *
 * @param state Desired state (C6WSH_DISABLED or C6WSH_ENABLED).
 * @return ESP_OK on successful NVS commit, or error code on flash write failure.
 */
esp_err_t nvram_set_c6wsh_state(c6wsh_state_t state);
/**
 * Frequency lower bound (80, 120, or 160 MHz).
 * In Fixed mode, min_freq == max_freq, locking the core at the exact setpoint.
 */
extern uint32_t ulp_me_min_freq;
