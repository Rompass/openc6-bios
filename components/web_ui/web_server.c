#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_partition.h>
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_rom_sys.h>
#include <nvs.h>
#include "cJSON.h"
#include "nvram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "me_shared.h"

static const char *TAG = "WEB_UI";

/* Embedded index.html boundaries (auto-generated via EMBED_FILES in CMake) */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

/**
 * @brief GET / - Serves the primary BIOS configuration HTML document.
 */
static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)index_html_start, index_html_end - index_html_start);
}

/**
 * @brief GET /api/sysinfo - Provides real-time hardware status metrics.
 */
static esp_err_t sysinfo_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    /* 1. Flash storage boundary audit */
    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        cJSON_AddNumberToObject(root, "flash_total_mb", flash_size / (1024 * 1024));
    }

    /* 2. Internal SRAM capacity and allocator headroom */
    uint32_t ram_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t ram_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    cJSON_AddNumberToObject(root, "ram_total_kb", ram_total / 1024);
    cJSON_AddNumberToObject(root, "ram_free_kb", ram_free / 1024);

    /* 3. Real-time CPU frequency (derived from PLL ticks) and ULP ME die temperature */
    cJSON_AddNumberToObject(root, "cpu_real_mhz", esp_rom_get_cpu_ticks_per_us());
    cJSON_AddNumberToObject(root, "cpu_temp_c", ulp_me_temperature);

    /* 4. Complete flash partition layout discovery */
    cJSON *parts = cJSON_AddArrayToObject(root, "partitions");
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (it != NULL) {
        const esp_partition_t *p = esp_partition_get(it);
        cJSON *part = cJSON_CreateObject();
        cJSON_AddStringToObject(part, "name", p->label);
        cJSON_AddNumberToObject(part, "offset", p->address);
        cJSON_AddNumberToObject(part, "size_kb", p->size / 1024);
        cJSON_AddItemToArray(parts, part);
        it = esp_partition_next(it);
    }
    esp_partition_iterator_release(it);

    /* 5. NVS storage utilization statistics */
    nvs_stats_t nvs_stats;
    if (nvs_get_stats(NULL, &nvs_stats) == ESP_OK) {
        cJSON_AddNumberToObject(root, "nvs_used_entries", nvs_stats.used_entries);
        cJSON_AddNumberToObject(root, "nvs_free_entries", nvs_stats.free_entries);
    }

    const char *json_str = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);

    free((void*)json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

/**
 * @brief GET /api/settings - Reads system configuration state from NVRAM to populate BIOS Setup UI.
 */
static esp_err_t settings_get_handler(httpd_req_t *req)
{
    dc_loss_action_t dc_action;
    post_led_mode_t led_mode;
    aura_mode_t aura_mode;
    uint8_t aura_br;
    cpu_freq_t cpu_freq;
    cpu_governor_t cpu_gov;
    bod_level_t bod_lvl;
    zc6_swap_mode_t zc6_swap;
    uint8_t temp_th;
    uint8_t temp_em;
    c6wsh_state_t c6wsh;
    char ssid[WIFI_SSID_MAX_LEN + 1] = {0};
    char pass[WIFI_PASS_MAX_LEN + 1] = {0};
    char country[WIFI_COUNTRY_MAX_LEN + 1] = {0};
    char pxe_url[PXE_URL_MAX_LEN + 1] = {0};

    nvram_get_dc_loss_action(&dc_action);
    nvram_get_post_led_mode(&led_mode);
    nvram_get_c6wsh_state(&c6wsh);
    nvram_get_wifi_sta_config(ssid, sizeof(ssid), pass, sizeof(pass));
    nvram_get_wifi_country(country, sizeof(country));
    nvram_get_pxe_url(pxe_url, sizeof(pxe_url));
    nvram_get_aura_mode(&aura_mode);
    nvram_get_aura_brightness(&aura_br);
    nvram_get_cpu_freq(&cpu_freq);
    nvram_get_cpu_governor(&cpu_gov);
    nvram_get_bod_level(&bod_lvl);
    nvram_get_zc6_swap_mode(&zc6_swap);
    nvram_get_thermal_limits(&temp_th, &temp_em);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "dc_loss", dc_action);
    cJSON_AddNumberToObject(root, "post_led", led_mode);
    cJSON_AddNumberToObject(root, "c6wsh", c6wsh);
    cJSON_AddStringToObject(root, "wifi_ssid", ssid);
    cJSON_AddStringToObject(root, "wifi_pass", pass);
    cJSON_AddStringToObject(root, "wifi_country", country);
    cJSON_AddStringToObject(root, "pxe_url", pxe_url);
    cJSON_AddNumberToObject(root, "aura_mode", aura_mode);
    cJSON_AddNumberToObject(root, "aura_brightness", aura_br);
    cJSON_AddNumberToObject(root, "cpu_freq", cpu_freq);
    cJSON_AddNumberToObject(root, "cpu_gov", cpu_gov);
    cJSON_AddNumberToObject(root, "bod_lvl", bod_lvl);
    cJSON_AddNumberToObject(root, "zc6_swap", zc6_swap);
    cJSON_AddNumberToObject(root, "temp_th", temp_th);
    cJSON_AddNumberToObject(root, "temp_em", temp_em);

    const char *json_str = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);

    free((void*)json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

/**
 * @brief POST /api/settings - Deserializes JSON configuration payload and commits parameters to NVRAM.
 */
static esp_err_t settings_post_handler(httpd_req_t *req)
{
    char buf[1024];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root) {
        cJSON *dc = cJSON_GetObjectItem(root, "dc_loss");
        cJSON *led = cJSON_GetObjectItem(root, "post_led");
        cJSON *wsh = cJSON_GetObjectItem(root, "c6wsh");
        cJSON *ssid = cJSON_GetObjectItem(root, "wifi_ssid");
        cJSON *pass = cJSON_GetObjectItem(root, "wifi_pass");
        cJSON *cc = cJSON_GetObjectItem(root, "wifi_country");
        cJSON *pxe = cJSON_GetObjectItem(root, "pxe_url");
        cJSON *aura = cJSON_GetObjectItem(root, "aura_mode");
        cJSON *br = cJSON_GetObjectItem(root, "aura_brightness");
        cJSON *freq = cJSON_GetObjectItem(root, "cpu_freq");
        cJSON *gov = cJSON_GetObjectItem(root, "cpu_gov");
        cJSON *bod = cJSON_GetObjectItem(root, "bod_lvl");
        cJSON *swap = cJSON_GetObjectItem(root, "zc6_swap");
        cJSON *th = cJSON_GetObjectItem(root, "temp_th");
        cJSON *em = cJSON_GetObjectItem(root, "temp_em");

        if (dc) nvram_set_dc_loss_action((dc_loss_action_t)dc->valueint);
        if (led) nvram_set_post_led_mode((post_led_mode_t)led->valueint);
        if (wsh) nvram_set_c6wsh_state((c6wsh_state_t)wsh->valueint);
        if (aura) nvram_set_aura_mode((aura_mode_t)aura->valueint);
        if (br) nvram_set_aura_brightness((uint8_t)br->valueint);
        if (freq) nvram_set_cpu_freq((cpu_freq_t)freq->valueint);
        if (gov) nvram_set_cpu_governor((cpu_governor_t)gov->valueint);
        if (bod) nvram_set_bod_level((bod_level_t)bod->valueint);
        if (swap) nvram_set_zc6_swap_mode((zc6_swap_mode_t)swap->valueint);

        if (th && em) {
            nvram_set_thermal_limits((uint8_t)th->valueint, (uint8_t)em->valueint);
        }

        if (ssid && pass) {
            nvram_set_wifi_sta_config(ssid->valuestring, pass->valuestring);
        }

        if (cc && cc->valuestring && strlen(cc->valuestring) >= 2) {
            nvram_set_wifi_country(cc->valuestring);
        }

        if (pxe && pxe->valuestring) {
            nvram_set_pxe_url(pxe->valuestring);
        }

        cJSON_Delete(root);
        ESP_LOGI(TAG, "BIOS Settings synchronized with NVRAM");
        return httpd_resp_sendstr(req, "OK");
    }

    httpd_resp_send_500(req);
    return ESP_FAIL;
}

/**
 * @brief POST /api/trigger_ota - Flags pending BIOS wireless OTA firmware state and resets SoC.
 */
static esp_err_t trigger_ota_handler(httpd_req_t *req)
{
    ESP_LOGW(TAG, "BIOS Network Update (OTA) triggered from Web UI!");

    esp_err_t err = nvram_set_bios_update_state(BIOS_UPDATE_PENDING);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set BIOS update pending state: %s", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "OK");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

static esp_err_t reboot_handler(httpd_req_t *req)
{
    ESP_LOGW(TAG, "Reboot command received...");
    httpd_resp_sendstr(req, "Rebooting...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

static esp_err_t defaults_handler(httpd_req_t *req)
{
    ESP_LOGE(TAG, "Load Defaults command received...");
    nvram_load_defaults();
    return httpd_resp_sendstr(req, "OK");
}

void web_ui_start(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.stack_size = 10240;

    ESP_LOGI(TAG, "Starting OpenC6 Web Interface on port %d", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t uri_root = { .uri = "/", .method = HTTP_GET, .handler = root_handler };
        httpd_register_uri_handler(server, &uri_root);

        httpd_uri_t uri_get = { .uri = "/api/settings", .method = HTTP_GET, .handler = settings_get_handler };
        httpd_register_uri_handler(server, &uri_get);

        httpd_uri_t uri_post = { .uri = "/api/settings", .method = HTTP_POST, .handler = settings_post_handler };
        httpd_register_uri_handler(server, &uri_post);

        httpd_uri_t uri_sys = { .uri = "/api/sysinfo", .method = HTTP_GET, .handler = sysinfo_get_handler };
        httpd_register_uri_handler(server, &uri_sys);

        httpd_uri_t uri_reboot = { .uri = "/api/reboot", .method = HTTP_POST, .handler = reboot_handler };
        httpd_register_uri_handler(server, &uri_reboot);

        httpd_uri_t uri_def = { .uri = "/api/defaults", .method = HTTP_POST, .handler = defaults_handler };
        httpd_register_uri_handler(server, &uri_def);

        httpd_uri_t uri_trig_ota = { .uri = "/api/trigger_ota", .method = HTTP_POST, .handler = trigger_ota_handler };
        httpd_register_uri_handler(server, &uri_trig_ota);
    }
}
