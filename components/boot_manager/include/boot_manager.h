#pragma once
#include <stdbool.h>

// Deployment target media for independent payloads
typedef enum {
    PAYLOAD_TARGET_RAM = 0,   // Deploy into free high-performance execution memory (IRAM)
    PAYLOAD_TARGET_FLASH = 1  // Deploy into persistent partition 'network_buf' for XIP execution
} payload_target_t;

// Interactive Boot Selection Matrix options
typedef enum {
    BOOT_OPT_NETWORK = 0,      // Wireless Network Boot (PXE)
    BOOT_OPT_SERIAL_RAM,       // Volatile Serial Boot (RAM Target)
    BOOT_OPT_SERIAL_FLASH,     // Non-Volatile Serial Boot (FLASH Target)
    BOOT_OPT_DEFAULT,          // Local OS execution from Flash Storage
    BOOT_OPT_SETUP,            // Open Local Configurator AP (Web UI Setup)
    BOOT_OPT_MAX               // Boundary index representing total options
} boot_option_t;

/**
 * @brief Initializes and runs the hardware interactive Boot Menu (driven by physical BOOT pin / GPIO 9).
 * Short press cycles through available options, while a long press (>=1s) selects the current target.
 * @return The selected boot option.
 */
boot_option_t boot_manager_interactive_menu(void);

/**
 * @brief Listens to the active communication interface (USB/UART) for a payload binary from the host PC.
 * @param timeout_sec The connection window duration in seconds before timing out.
 * @param target The target destination where the streamed payload binary will be written.
 * @return true if the payload has been successfully parsed, validated, and mapped.
 */
bool boot_manager_serial_listen(int timeout_sec, payload_target_t target);
