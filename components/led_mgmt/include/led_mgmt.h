#pragma once

#include <stdint.h>
#include "nvram_schema.h" // Required to access the aura_mode_t type

// System status color representations (R, G, B)
#define COLOR_POST_OK    0, 255, 0   // Green
#define COLOR_POST_ERROR 255, 0, 0   // Red
#define COLOR_BIOS_SETUP 0, 0, 255   // Blue

/**
 * @brief Initializes the addressable RGB status LED hardware interface.
 */
void led_mgmt_init(void);

/**
 * @brief Sets a static solid color (handles GBR color mapping conversion).
 */
void led_mgmt_set_color(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Blinks the status LED to indicate POST phase progress.
 */
void led_mgmt_blink_post(uint8_t r, uint8_t g, uint8_t b, int count);

/**
 * @brief Manages the background Aura Sync effect (Rainbow Flow).
 * @param mode AURA_RAINBOW to launch or AURA_DISABLED to terminate.
 */
void led_mgmt_set_aura_mode(aura_mode_t mode);
