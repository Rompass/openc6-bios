#include "pxe_boot.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sandbox.h"
#include "openc6_fs.h"
#include "hal_flash.h"
#include "hw_usb.h"

static const char *TAG = "PXE_BOOT";

/**
 * @brief Extracts the file name from a complete URL path.
 */
static const char *get_filename_from_url(const char *url)
{
    const char *filename = strrchr(url, '/');
    if (filename && *(filename + 1) != '\0') {
        return filename + 1;
    }
    return "payload.bin";
}

/**
 * @brief Ensures target virtual directory exists before writing payload.
 */
static uint16_t get_or_create_dir(const char *dir_name)
{
    int16_t id = fs_find_id(dir_name, 0);
    if (id >= 0) {
        return (uint16_t)id;
    }

    id = fs_mkdir(dir_name, 0);
    if (id >= 0) {
        return (uint16_t)id;
    }

    return 0;
}

bool pxe_boot_execute(const char* url)
{
    if (!url || strlen(url) == 0) {
        hw_usb_print("[PXE] Error: Invalid URL specified.\r\n");
        return false;
    }

    /* Normalize URL: ensure path exists to avoid NULL dereference inside esp_http_client */
    char full_url[192];
    strncpy(full_url, url, sizeof(full_url) - 1);
    full_url[sizeof(full_url) - 1] = '\0';

    /* If URL has no path after host/port (e.g. "http://192.168.1.1:8080"), append "/payload.bin" */
    char *proto_end = strstr(full_url, "://");
    char *first_slash = proto_end ? strchr(proto_end + 3, '/') : strchr(full_url, '/');
    if (!first_slash) {
        strncat(full_url, "/payload.bin", sizeof(full_url) - strlen(full_url) - 1);
    }

    char log_msg[256];
    snprintf(log_msg, sizeof(log_msg), "[PXE] Normalized URL: %s\r\n", full_url);
    hw_usb_print(log_msg);

    uint8_t *arena = sandbox_get_arena();
    size_t arena_size = sandbox_get_arena_size();

    if (!arena || arena_size == 0) {
        hw_usb_print("[PXE] Error: Dynamic Sandbox arena not allocated.\r\n");
        return false;
    }

    esp_http_client_config_t config = {
        .url = full_url,
        .timeout_ms = 7000,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        hw_usb_print("[PXE] Error: Failed to initialize HTTP client.\r\n");
        return false;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        char err_msg[64];
        snprintf(err_msg, sizeof(err_msg), "[PXE] Error opening connection: %s\r\n", esp_err_to_name(err));
        hw_usb_print(err_msg);
        esp_http_client_cleanup(client);
        return false;
    }

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length <= 0) {
        hw_usb_print("[PXE] Error: Empty file or chunked encoding unsupported.\r\n");
        esp_http_client_cleanup(client);
        return false;
    }

    if ((size_t)content_length > arena_size) {
        char sz_msg[96];
        snprintf(sz_msg, sizeof(sz_msg), "[PXE] Error: File (%d bytes) exceeds arena capacity (%zu bytes)!\r\n",
                 content_length, arena_size);
        hw_usb_print(sz_msg);
        esp_http_client_cleanup(client);
        return false;
    }

    char start_msg[96];
    snprintf(start_msg, sizeof(start_msg), "[PXE] Streaming %d bytes into Sandbox arena...\r\n", content_length);
    hw_usb_print(start_msg);

    /* Stream directly into pre-allocated sandbox arena memory to save task stack */
    int total_read = 0;
    while (total_read < content_length) {
        int read_len = esp_http_client_read(client, (char *)(arena + total_read), content_length - total_read);
        if (read_len < 0) {
            hw_usb_print("[PXE] Error reading network stream.\r\n");
            esp_http_client_cleanup(client);
            return false;
        }
        if (read_len == 0) {
            break;
        }

        total_read += read_len;
    }

    esp_http_client_cleanup(client);

    /* Allow lwIP and Wi-Fi radio to flush TCP FIN/ACK packets before disabling flash cache */
    vTaskDelay(pdMS_TO_TICKS(50));

    if (total_read == content_length) {
        uint16_t target_dir_id = get_or_create_dir("downloaded");
        const char *filename = get_filename_from_url(full_url);

        char save_msg[96];
        snprintf(save_msg, sizeof(save_msg), "[PXE] Saving '%s' to /downloaded/ (Dir %u)...\r\n", filename, target_dir_id);
        hw_usb_print(save_msg);

        if (fs_write_file(filename, arena, total_read, target_dir_id) < 0) {
            hw_usb_print("[PXE] Error: File system write failed. Storage may be full.\r\n");
            return false;
        }

        hw_usb_print("[PXE] Network boot image successfully deployed to flash!\r\n");
        return true;
    }

    hw_usb_print("[PXE] Error: Download stream truncated.\r\n");
    return false;
}

bool pxe_bios_ota_execute(const char* url)
{
    ESP_LOGW(TAG, "Starting Network BIOS OTA from: %s", url);

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 7000,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return false;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length <= 0) {
        ESP_LOGE(TAG, "Failed to get Content-Length or file is empty! (Size: %d)", content_length);
        esp_http_client_cleanup(client);
        return false;
    }

    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
    if (update_part == NULL) {
        ESP_LOGE(TAG, "Passive OTA partition not found!");
        esp_http_client_cleanup(client);
        return false;
    }

    ESP_LOGI(TAG, "Writing BIOS OTA to partition: %s at offset 0x%08lX", update_part->label, update_part->address);

    esp_ota_handle_t update_handle = 0;
    err = esp_ota_begin(update_part, OTA_SIZE_UNKNOWN, &update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }

    /* Allocate dedicated 4 KB staging buffer on system heap instead of uninitialized arena */
    const size_t chunk_size = 4096;
    uint8_t *ota_buf = (uint8_t *)malloc(chunk_size);
    if (!ota_buf) {
        ESP_LOGE(TAG, "Failed to allocate 4KB OTA staging buffer");
        esp_ota_abort(update_handle);
        esp_http_client_cleanup(client);
        return false;
    }

    int total_read = 0;

    while (total_read < content_length) {
        int to_read = (content_length - total_read > (int)chunk_size) ? (int)chunk_size : (content_length - total_read);
        int read_len = esp_http_client_read(client, (char *)ota_buf, to_read);
        if (read_len < 0) {
            ESP_LOGE(TAG, "Error reading network stream");
            free(ota_buf);
            esp_ota_abort(update_handle);
            esp_http_client_cleanup(client);
            return false;
        }
        if (read_len == 0) {
            break;
        }

        err = esp_ota_write(update_handle, ota_buf, read_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            free(ota_buf);
            esp_ota_abort(update_handle);
            esp_http_client_cleanup(client);
            return false;
        }

        total_read += read_len;
    }

    free(ota_buf);
    esp_http_client_cleanup(client);

    if (total_read == content_length) {
        err = esp_ota_end(update_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
            return false;
        }

        err = esp_ota_set_boot_partition(update_part);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
            return false;
        }

        ESP_LOGI(TAG, "BIOS OTA Flashed successfully! Total: %d bytes", total_read);
        return true;
    }

    ESP_LOGE(TAG, "OTA Download incomplete! Got %d out of %d", total_read, content_length);
    esp_ota_abort(update_handle);
    return false;
}
