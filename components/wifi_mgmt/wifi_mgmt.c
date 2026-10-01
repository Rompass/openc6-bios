#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "wifi_mgmt.h"
#include "nvram.h"
#include "management_engine.h"
#include "hw_usb.h"
#include "led_mgmt.h"

static EventGroupHandle_t s_wifi_event_group = NULL;
static int s_retry_count = 0;
static bool s_sta_started = false;
static bool s_reconnect_allowed = false;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define MAX_RETRY_ATTEMPTS 5

static const char* wifi_auth_to_str(wifi_auth_mode_t auth)
{
    switch (auth) {
        case WIFI_AUTH_OPEN:            return "OPEN";
        case WIFI_AUTH_WEP:             return "WEP";
        case WIFI_AUTH_WPA_PSK:         return "WPA";
        case WIFI_AUTH_WPA2_PSK:        return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
        case WIFI_AUTH_WPA3_PSK:        return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
        default:                        return "UNKNOWN";
    }
}

static const char* wifi_reason_to_str(uint8_t reason)
{
    switch (reason) {
        case 1:   return "UNSPECIFIED";
        case 2:   return "AUTH_EXPIRE";
        case 3:   return "AUTH_LEAVE";
        case 4:   return "ASSOC_EXPIRE";
        case 5:   return "ASSOC_TOOMANY";
        case 6:   return "NOT_AUTHED";
        case 7:   return "NOT_ASSOCED";
        case 8:   return "ASSOC_LEAVE";
        case 15:  return "4WAY_HANDSHAKE_TIMEOUT";
        case 201: return "NO_AP_FOUND";
        case 202: return "AUTH_FAIL";
        case 203: return "ASSOC_FAIL";
        case 204: return "HANDSHAKE_TIMEOUT";
        case 205: return "CONNECTION_FAIL";
        default:  return "UNKNOWN_REASON";
    }
}

static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            hw_usb_print("[WIFI_EVT] STA_START: Radio active\r\n");
        } else if (event_id == WIFI_EVENT_STA_CONNECTED) {
            hw_usb_print("[WIFI_EVT] STA_CONNECTED: L2 Link established!\r\n");
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            wifi_event_sta_disconnected_t* disconn = (wifi_event_sta_disconnected_t*)event_data;
            char dbg[96];
            snprintf(dbg, sizeof(dbg), "[WIFI_EVT] DISCONNECTED! Code: %u (%s) [Retry %d/%d]\r\n",
                     disconn->reason, wifi_reason_to_str(disconn->reason),
                     s_retry_count, MAX_RETRY_ATTEMPTS);
            hw_usb_print(dbg);

            if (s_reconnect_allowed && (s_retry_count < MAX_RETRY_ATTEMPTS)) {
                s_retry_count++;
                esp_wifi_connect();
            } else {
                if (s_wifi_event_group) {
                    xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                }
            }
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
            char ip_msg[64];
            snprintf(ip_msg, sizeof(ip_msg), "[WIFI_EVT] GOT_IP: " IPSTR "\r\n", IP2STR(&event->ip_info.ip));
            hw_usb_print(ip_msg);

            s_retry_count = 0;
            if (s_wifi_event_group) {
                xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            }
        }
    }
}

void wifi_mgmt_init(void)
{
    static bool initialized = false;
    if (initialized) {
        return;
    }

    esp_netif_init();
    esp_event_loop_create_default();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    /* Retrieve regulatory country code from NVRAM */
    char country_code[WIFI_COUNTRY_MAX_LEN + 1] = {0};
    nvram_get_wifi_country(country_code, sizeof(country_code));

    uint8_t max_channels = 13;
    if (strcmp(country_code, "US") == 0) {
        max_channels = 11;
    } else if (strcmp(country_code, "JP") == 0) {
        max_channels = 14;
    }

    /* Configure regulatory domain with auto-adaptation to beacon IE */
    wifi_country_t country = {
        .schan = 1,
        .nchan = max_channels,
        .policy = WIFI_COUNTRY_POLICY_AUTO
    };
    strncpy(country.cc, country_code, sizeof(country.cc) - 1);
    esp_wifi_set_country(&country);

    if (s_wifi_event_group == NULL) {
        s_wifi_event_group = xEventGroupCreate();
    }

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL);

    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_netif == NULL) {
        esp_netif_create_default_wifi_sta();
    }

    initialized = true;
}

static void ensure_sta_mode(void)
{
    if (!s_sta_started) {
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_start();
        s_sta_started = true;
    }
}

/**
 * @brief Standalone RF spectrum scan. Outputs formatted table of detected APs.
 */
esp_err_t wifi_mgmt_scan(void)
{
    ensure_sta_mode();

    hw_usb_print("[WIFI] Scanning 2.4 GHz spectrum (Channels 1-13)...\r\n");

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 150
    };

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        esp_wifi_scan_stop();
        vTaskDelay(pdMS_TO_TICKS(100));
        err = esp_wifi_scan_start(&scan_cfg, true);
        if (err != ESP_OK) {
            hw_usb_print("[WIFI] Error: RF scan failed.\r\n");
            return err;
        }
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);

    if (ap_count == 0) {
        hw_usb_print("[WIFI] No access points detected.\r\n");
        return ESP_OK;
    }

    wifi_ap_record_t *records = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * ap_count);
    if (!records) {
        hw_usb_print("[WIFI] Error: Insufficient memory for scan results.\r\n");
        return ESP_ERR_NO_MEM;
    }

    esp_wifi_scan_get_ap_records(&ap_count, records);

    hw_usb_print("\r\n=========================================================================\r\n");
    hw_usb_print(" #  | SSID                             | RSSI    | CH | Auth\r\n");
    hw_usb_print("=========================================================================\r\n");

    for (int i = 0; i < ap_count && i < 25; i++) {
        char row[128];
        snprintf(row, sizeof(row), " %-2d | %-32.32s | %-4d dBm | %-2d | %s\r\n",
                 i + 1,
                 strlen((char *)records[i].ssid) > 0 ? (char *)records[i].ssid : "<Hidden SSID>",
                 records[i].rssi,
                 records[i].primary,
                 wifi_auth_to_str(records[i].authmode));
        hw_usb_print(row);
        management_engine_pet_watchdog();
    }
    hw_usb_print("=========================================================================\r\n\r\n");

    free(records);
    return ESP_OK;
}

/**
 * @brief Universal station connection service with optional NVRAM credential preservation.
 */
esp_err_t wifi_mgmt_connect(const char* ssid, const char* pass, bool save_nvram)
{
    if (!ssid || strlen(ssid) == 0) {
        hw_usb_print("[WIFI] Error: Empty SSID.\r\n");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_wifi_event_group == NULL) {
        s_wifi_event_group = xEventGroupCreate();
    }

    ensure_sta_mode();

    s_reconnect_allowed = false;
    s_retry_count = 0;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    /* If already connected to this SSID, check if IP is valid */
    wifi_ap_record_t current_ap;
    if (esp_wifi_sta_get_ap_info(&current_ap) == ESP_OK) {
        if (strcmp((char *)current_ap.ssid, ssid) == 0 && wifi_mgmt_is_connected()) {
            hw_usb_print("[WIFI] Already connected and holding valid L3 IP.\r\n");
            return ESP_OK;
        }
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    /* Configure station credentials */
    wifi_config_t wifi_config;
    memset(&wifi_config, 0, sizeof(wifi_config));
    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);

    if (pass && strlen(pass) > 0) {
        strncpy((char*)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password) - 1);
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_set_ps(WIFI_PS_NONE);

    s_reconnect_allowed = true;
    char conn_msg[96];
    snprintf(conn_msg, sizeof(conn_msg), "[WIFI] Connecting to '%s'...\r\n", ssid);
    hw_usb_print(conn_msg);

    esp_wifi_connect();

    /* Await DHCP resolution (12s limit) */
    uint64_t start_time = esp_timer_get_time();
    while ((esp_timer_get_time() - start_time) < 12000000ULL) {
        management_engine_pet_watchdog();

        EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
        if (bits & WIFI_CONNECTED_BIT) {
            hw_usb_print("[WIFI] Connected successfully!\r\n");
            if (save_nvram) {
                nvram_set_wifi_sta_config(ssid, pass ? pass : "");
                hw_usb_print("[WIFI] Credentials saved to NVRAM.\r\n");
            }
            wifi_mgmt_print_status();
            return ESP_OK;
        }
        if (bits & WIFI_FAIL_BIT) {
            hw_usb_print("[WIFI] Error: Connection rejected by Access Point.\r\n");
            s_reconnect_allowed = false;
            esp_wifi_disconnect();
            return ESP_FAIL;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    hw_usb_print("[WIFI] Error: Connection timeout.\r\n");
    s_reconnect_allowed = false;
    esp_wifi_disconnect();
    return ESP_ERR_TIMEOUT;
}

esp_err_t wifi_mgmt_connect_direct(const char* ssid, const char* pass)
{
    return wifi_mgmt_connect(ssid, pass, false);
}

esp_err_t wifi_mgmt_disconnect(void)
{
    s_reconnect_allowed = false;
    s_retry_count = 0;
    if (s_wifi_event_group) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
    esp_err_t err = esp_wifi_disconnect();
    hw_usb_print("[WIFI] Station disconnected.\r\n");
    return err;
}

void wifi_mgmt_print_status(void)
{
    hw_usb_print("\r\n--- Network Interface Status ---\r\n");

    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    char mac_str[48];
    snprintf(mac_str, sizeof(mac_str), "MAC Address : %02X:%02X:%02X:%02X:%02X:%02X\r\n",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    hw_usb_print(mac_str);

    if (!wifi_mgmt_is_connected()) {
        hw_usb_print("Status      : Disconnected\r\n");
        return;
    }

    hw_usb_print("Status      : Connected (L3 Active)\r\n");

    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        char ap_str[192];
        snprintf(ap_str, sizeof(ap_str), "SSID        : %s\r\nChannel     : %d\r\nSignal RSSI : %d dBm\r\nAuth Mode   : %s\r\n",
                 (char *)ap_info.ssid, ap_info.primary, ap_info.rssi, wifi_auth_to_str(ap_info.authmode));
        hw_usb_print(ap_str);
    }

    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK) {
            char net_str[128];
            snprintf(net_str, sizeof(net_str), "IPv4 Address: " IPSTR "\r\nSubnet Mask : " IPSTR "\r\nGateway     : " IPSTR "\r\n",
                     IP2STR(&ip_info.ip), IP2STR(&ip_info.netmask), IP2STR(&ip_info.gw));
            hw_usb_print(net_str);
        }
    }
    hw_usb_print("--------------------------------\r\n\r\n");
}

esp_err_t wifi_mgmt_start_ap(const char* ssid, const char* pass)
{
    s_sta_started = false;

    if (s_wifi_event_group) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    }

    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif == NULL) {
        esp_netif_create_default_wifi_ap();
    }

    esp_wifi_stop();

    wifi_config_t wifi_config;
    memset(&wifi_config, 0, sizeof(wifi_config));
    wifi_config.ap.ssid_len = strlen(ssid);
    wifi_config.ap.channel = 1;
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = (strlen(pass) == 0) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    wifi_config.ap.pmf_cfg.required = false;

    strncpy((char*)wifi_config.ap.ssid, ssid, sizeof(wifi_config.ap.ssid) - 1);
    strncpy((char*)wifi_config.ap.password, pass, sizeof(wifi_config.ap.password) - 1);

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) return err;

    err = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (err != ESP_OK) return err;

    err = esp_wifi_start();
    if (err != ESP_OK) return err;

    return ESP_OK;
}

esp_err_t wifi_mgmt_start_sta(void)
{
    char ssid[WIFI_SSID_MAX_LEN + 1] = {0};
    char pass[WIFI_PASS_MAX_LEN + 1] = {0};
    nvram_get_wifi_sta_config(ssid, sizeof(ssid), pass, sizeof(pass));

    if (strlen(ssid) == 0) {
        hw_usb_print("[WIFI] Warning: No Wi-Fi credentials stored in NVRAM.\r\n");
        return ESP_ERR_NOT_FOUND;
    }

    return wifi_mgmt_connect(ssid, pass, false);
}

bool wifi_mgmt_is_connected(void)
{
    if (!s_wifi_event_group) {
        return false;
    }
    return (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) != 0;
}
