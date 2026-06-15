#pragma once
#include <stdbool.h>

/**
 * @brief Initiates an HTTP download of a binary payload from the network into the 'network_buf' partition.
 * @param url HTTP file address (e.g., "http://192.168.1.1:8080/payload.bin")
 * @return true if download and flash write operations complete successfully.
 */
bool pxe_boot_execute(const char* url);
bool pxe_bios_ota_execute(const char* url);
