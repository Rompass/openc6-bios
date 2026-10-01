#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "openc6_abi.h"

#ifdef __cplusplus
extern "C" {
    #endif

    /**
     * @brief Starts the OpenC6 BIOS v2.0 Web Setup Portal.
     * Initializes the background HTTP daemon on port 80 to serve the BIOS configuration
     * utility and REST API when the system enters AP setup mode.
     */
    void web_ui_start(void);

    /**
     * @brief Initializes and launches the remote Web Shell (c6wsh) daemon.
     * Creates an HTTP server on port 80 over the established station (STA) network interface.
     * Allocates a ring buffer to stream direct UART/USB console output and listens for
     * remote terminal execution commands.
     *
     * @param abi Pointer to the OpenC6 system ABI dispatch jump-table.
     * @return ESP_OK on successful listener binding, or ESP-IDF error code on socket/memory failure.
     */
    esp_err_t c6wsh_start(const openc6_abi_t *abi);

    /**
     * @brief Terminates the active c6wsh daemon and releases network resources.
     * Tears down HTTP listener handles and frees internal ring buffer heap allocations
     * so that full SRAM is restored for subsequent execution tasks.
     */
    void c6wsh_stop(void);

    /**
     * @brief Ingests raw serial/UART console bytes into the c6wsh streaming pipe.
     * Pushes console output emitted by the BIOS, RTOS, or sandboxed payloads into a
     * thread-safe ring buffer, immediately broadcasting it to active HTTP stream listeners.
     *
     * @param data Pointer to transmitted character or byte buffer.
     * @param len Number of bytes to pipe into the stream.
     */
    void c6wsh_write_output(const uint8_t *data, size_t len);

    /**
     * @brief Queries whether the c6wsh remote terminal service is actively running.
     * Used by low-level serial drivers to decide whether to duplicate outgoing UART
     * characters into the network buffer.
     *
     * @return true if c6wsh is running and accepting connections, false otherwise.
     */
    bool c6wsh_is_active(void);

    #ifdef __cplusplus
}
#endif
