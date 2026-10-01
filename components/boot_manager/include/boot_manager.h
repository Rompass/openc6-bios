#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "openc6_abi.h"

/* Target execution medium for binary payloads */
typedef enum {
    PAYLOAD_TARGET_RAM = 0,
    PAYLOAD_TARGET_FLASH = 1
} payload_target_t;

/* Simplified interactive boot menu targets (GPIO 9 button) */
typedef enum {
    BOOT_OPT_DEFAULT = 0, /* Auto-boot payload.bin from OpenC6 FS */
    BOOT_OPT_SHELL   = 1, /* Interactive Micro UNIX Shell */
    BOOT_OPT_SETUP   = 2, /* Web UI BIOS Setup Utility */
    BOOT_OPT_MAX     = 3
} boot_option_t;

/**
 * @brief Renders interactive boot selection via physical BOOT button (GPIO 9).
 *
 * @return boot_option_t Selected boot action.
 */
boot_option_t boot_manager_interactive_menu(void);

/**
 * @brief Streams and validates a binary payload over serial (USB Type-C).
 *
 * @param timeout_sec Transfer listen timeout in seconds.
 * @param target Destination memory target.
 * @return true on successful deployment and execution.
 */
bool boot_manager_serial_listen(int timeout_sec, payload_target_t target);

/**
 * @brief Launches interactive micro UNIX shell over native USB Type-C interface.
 *
 * @param abi Pointer to global BIOS ABI jump-table.
 */
void boot_manager_shell(const openc6_abi_t *abi);
