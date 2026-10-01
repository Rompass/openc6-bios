#pragma once

#include <stdint.h>
#include "nvram_schema.h"

/* System status color representations (R, G, B) */
#define COLOR_POST_OK    0, 255, 0   /* Green: Hardware self-test passed */
#define COLOR_POST_ERROR 255, 0, 0   /* Red: Hardware or NVRAM fault */
#define COLOR_BIOS_SETUP 0, 0, 255   /* Blue: BIOS Web Setup portal active */

/**
 * @brief Initializes the addressable WS2812 RGB LED GPIO pad and runs a power-on color test.
 * Clears sleep hold latches, configures active pulldown, and asserts output low to prevent latch-up.
 */
void led_mgmt_init(void);

/**
 * @brief Sets a solid static RGB color directly on the WS2812 status LED.
 * Automatically scales red, green, and blue intensities by the active NVRAM brightness setting.
 * When (0, 0, 0) is commanded, latches GPIO at 0V using hardware pad hold to prevent sleep leakage.
 *
 * @param r Red channel intensity (0-255).
 * @param g Green channel intensity (0-255).
 * @param b Blue channel intensity (0-255).
 */
void led_mgmt_set_color(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Blinks the status LED synchronously to display POST code sequences.
 * Bypasses the active Aura background task during diagnostics.
 *
 * @param r Red channel intensity.
 * @param g Green channel intensity.
 * @param b Blue channel intensity.
 * @param count Number of blink cycles to execute.
 */
void led_mgmt_blink_post(uint8_t r, uint8_t g, uint8_t b, int count);

/**
 * @brief Controls the Aura Sync lighting engine (Static Blue, Rainbow Flow, or Disabled).
 * Spawns or tears down the background FreeRTOS effect task based on the specified mode.
 *
 * @param mode Target profile (AURA_STATIC, AURA_RAINBOW, or AURA_DISABLED).
 */
void led_mgmt_set_aura_mode(aura_mode_t mode);

/**
 * @brief Loads the configured Aura profile and brightness level directly from NVRAM.
 * Restores the user's lighting preferences after POST diagnostics or payload execution.
 */
void led_mgmt_apply_aura_from_nvram(void);
