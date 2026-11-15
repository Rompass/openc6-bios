#pragma once

/*
 * ============================================================================
 * OpenC6 Board Pinout & Hardware Capability Map (ESP32-C6-Zero)
 * Pure preprocessor definitions (safe for both HP-Core and LP-Core builds).
 * ============================================================================
 */

/* Hardware capabilities of this target */
#define HAL_CAP_LP_CORE         1
#define HAL_CAP_THERMAL_SENSOR  1
#define HAL_CAP_BOD             1
#define HAL_CAP_WS2812          1

/* Hardware Clear CMOS / NVRAM Factory Reset jumper */
#define HAL_PIN_CMOS_GND        1
#define HAL_PIN_CLEAR_NVRAM     2

/* Power button & Management Engine LP-Core sense line */
#define HAL_PIN_PWR_BTN_GND     3
#define HAL_PIN_PWR_BTN_SENSE   4

/* Onboard WS2812 Addressable RGB LED */
#define HAL_PIN_WS2812          8

/* Physical BOOT Button & Strapping pin */
#define HAL_PIN_BOOT_BTN        9

/* USB-Serial-JTAG Native D- / D+ PHY lines */
#define HAL_PIN_USB_DM          12
#define HAL_PIN_USB_DP          13

/*
 * Protected Pin Bitmask for ESP32-C6-Zero:
 * - GPIO 1, 2   : Clear CMOS / NVRAM Factory Reset jumper lines
 * - GPIO 3, 4   : Management Engine LP-Core GND / Sense line & Wakeup
 * - GPIO 8      : Onboard WS2812 Addressable RGB LED (Exclusive to led_mgmt)
 * - GPIO 9      : Physical BOOT Button & Strapping pin
 * - GPIO 12, 13 : USB-Serial-JTAG Native D- / D+ PHY lines
 * - GPIO 24..30 : Internal High-Speed SPI Flash bus
 */
#define HAL_GPIO_PROTECTED_MASK ( \
(1UL << HAL_PIN_CMOS_GND)      | \
(1UL << HAL_PIN_CLEAR_NVRAM)   | \
(1UL << HAL_PIN_PWR_BTN_GND)   | \
(1UL << HAL_PIN_PWR_BTN_SENSE) | \
(1UL << HAL_PIN_WS2812)        | \
(1UL << HAL_PIN_BOOT_BTN)      | \
(1UL << HAL_PIN_USB_DM)        | \
(1UL << HAL_PIN_USB_DP)        | \
(0x7FUL << 24) \
)
