#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define WIFI_SSID_MAX_LEN 32
#define WIFI_PASS_MAX_LEN 64

#ifdef __cplusplus
extern "C" {
    #endif

    void      wifi_mgmt_init(void);
    esp_err_t wifi_mgmt_scan(void);
    esp_err_t wifi_mgmt_connect(const char* ssid, const char* pass, bool save_nvram);
    esp_err_t wifi_mgmt_connect_direct(const char* ssid, const char* pass);
    esp_err_t wifi_mgmt_disconnect(void);
    void      wifi_mgmt_print_status(void);
    bool      wifi_mgmt_is_connected(void);
    esp_err_t wifi_mgmt_start_sta(void);
    esp_err_t wifi_mgmt_start_ap(const char* ssid, const char* pass);

    #ifdef __cplusplus
}
#endif
