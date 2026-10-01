#pragma once

#include <stdint.h>
#include <stdbool.h>

/*
 * ============================================================================
 * OpenC6 Bare-Metal GPIO Hardware Abstraction Layer (ESP32-C6-Zero)
 * Prefixed register names to avoid collisions with ESP-IDF soc/reg_base.h.
 * ============================================================================
 */

/* ESP32-C6 HP GPIO Hardware Base Registers (TRM Rev 1.2, Ch. 7) */
#define C6_GPIO_BASE             0x60091000UL
#define C6_GPIO_OUT_W1TS_REG     ((volatile uint32_t *)(C6_GPIO_BASE + 0x0008))
#define C6_GPIO_OUT_W1TC_REG     ((volatile uint32_t *)(C6_GPIO_BASE + 0x000C))
#define C6_GPIO_ENABLE_W1TS_REG  ((volatile uint32_t *)(C6_GPIO_BASE + 0x0024))
#define C6_GPIO_ENABLE_W1TC_REG  ((volatile uint32_t *)(C6_GPIO_BASE + 0x0028))
#define C6_GPIO_IN_REG           ((volatile uint32_t *)(C6_GPIO_BASE + 0x003C))

/* IO MUX Base Register for pad function assignment */
#define C6_IO_MUX_BASE           0x60090000UL
#define C6_IO_MUX_PIN_REG(pin)   ((volatile uint32_t *)(C6_IO_MUX_BASE + 0x0004 + ((pin) * 4)))
#define C6_IO_MUX_MCU_SEL_GPIO   (1UL << 12) /* Function 1: Direct GPIO Pad Select */

/*
 * Protected Pin Bitmask for ESP32-C6-Zero:
 * - GPIO 1, 2   : Hardware Clear CMOS / NVRAM Factory Reset jumper lines
 * - GPIO 3, 4   : Management Engine LP-Core GND / Sense line & Wakeup
 * - GPIO 8      : Onboard WS2812 Addressable RGB LED (Exclusive to led_mgmt)
 * - GPIO 9      : Physical BOOT Button & Strapping pin
 * - GPIO 12, 13 : USB-Serial-JTAG Native D- / D+ PHY lines
 * - GPIO 24..30 : Internal High-Speed SPI Flash bus
 */
#define OPENC6_GPIO_PROTECTED_MASK ( \
(1UL << 1)  | \
(1UL << 2)  | \
(1UL << 3)  | \
(1UL << 4)  | \
(1UL << 8)  | \
(1UL << 9)  | \
(1UL << 12) | \
(1UL << 13) | \
(0x7FUL << 24) \
)

/**
 * @brief Checks if a GPIO pin is permissible for unprivileged user operations.
 *
 * @param pin Target GPIO pin number (0..30).
 * @return true if pin is accessible, false if reserved/invalid.
 */
static inline bool hal_gpio_is_safe(uint32_t pin)
{
    if (pin > 30) {
        return false;
    }
    return ((OPENC6_GPIO_PROTECTED_MASK & (1UL << pin)) == 0);
}

/**
 * @brief Configures pin direction as Input or Output using atomic registers.
 *
 * @param pin Target GPIO pin index.
 * @param is_output 1 for Output, 0 for Input.
 * @return 0 on success, -1 if pin is protected or invalid.
 */
static inline int32_t hal_gpio_set_direction(uint32_t pin, uint32_t is_output)
{
    if (!hal_gpio_is_safe(pin)) {
        return -1;
    }

    /* Route pad directly to GPIO peripheral matrix (Function 1) */
    *C6_IO_MUX_PIN_REG(pin) = (*C6_IO_MUX_PIN_REG(pin) & ~(0x7UL << 12)) | C6_IO_MUX_MCU_SEL_GPIO;

    if (is_output) {
        *C6_GPIO_ENABLE_W1TS_REG = (1UL << pin);
    } else {
        *C6_GPIO_ENABLE_W1TC_REG = (1UL << pin);
    }

    return 0;
}

/**
 * @brief Sets output logic level using write-1-to-set and write-1-to-clear registers.
 * Guarantees zero race conditions without disabling CPU interrupts.
 *
 * @param pin Target GPIO pin index.
 * @param level 1 for HIGH (3.3V), 0 for LOW (0V).
 * @return 0 on success, -1 if pin is protected or invalid.
 */
static inline int32_t hal_gpio_write_pin(uint32_t pin, uint32_t level)
{
    if (!hal_gpio_is_safe(pin)) {
        return -1;
    }

    if (level) {
        *C6_GPIO_OUT_W1TS_REG = (1UL << pin);
    } else {
        *C6_GPIO_OUT_W1TC_REG = (1UL << pin);
    }

    return 0;
}

/**
 * @brief Reads digital logic level from bare-metal input register.
 *
 * @param pin Target GPIO pin index.
 * @return 0 or 1 on success, -1 if pin is protected or invalid.
 */
static inline int32_t hal_gpio_read_pin(uint32_t pin)
{
    if (!hal_gpio_is_safe(pin)) {
        return -1;
    }

    return (*C6_GPIO_IN_REG & (1UL << pin)) ? 1 : 0;
}
