#pragma once
#include "esp_err.h"

// Initialize the Wi-Fi network stack (call once on startup)
void wifi_mgmt_init(void);

// Start in Access Point mode (for Web UI Configurator)
esp_err_t wifi_mgmt_start_ap(const char* ssid, const char* pass);

// Start in Station/Client mode (for wireless PXE Boot and OTA services)
esp_err_t wifi_mgmt_start_sta(void);

// Verify current connection status with the target Access Point
bool wifi_mgmt_is_connected(void);
