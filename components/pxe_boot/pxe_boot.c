#include "pxe_boot.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include <string.h>
#include <stdlib.h>

#include "openc6_fs.h"
#include "hal_flash.h"

static const char *TAG = "PXE_BOOT";

// Helper to extract filename from URL (e.g. "http://host/app.bin" -> "app.bin")
static const char *get_filename_from_url(const char *url) {
    const char *filename = strrchr(url, '/');
    if (filename) {
        return filename + 1;
    }
    return "payload.bin"; // fallback name
}

static uint16_t get_or_create_dir(const char *dir_name) {
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

// ─── 1. USER PAYLOAD NETWORK DEPLOYMENT TO /downloaded/ FOLDER (RAM Staging) ───

bool pxe_boot_execute(const char* url) {
    ESP_LOGI(TAG, "Starting Network Boot from: %s", url);

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 5000,
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

    // Limit maximum payload download size to protect SRAM boundaries (e.g. max 150 KB)
    if (content_length > 150 * 1024) {
        ESP_LOGE(TAG, "Payload size %d exceeds safe RAM limits (150KB)!", content_length);
        esp_http_client_cleanup(client);
        return false;
    }

    ESP_LOGI(TAG, "Found payload. Size: %d bytes. Allocating RAM buffer...", content_length);

    uint8_t *h_buf = malloc(content_length);
    if (!h_buf) {
        ESP_LOGE(TAG, "Failed to allocate memory buffer of %d bytes", content_length);
        esp_http_client_cleanup(client);
        return false;
    }

    ESP_LOGI(TAG, "Downloading payload stream...");

    int total_read = 0;
    char buffer[1024];

    while (total_read < content_length) {
        int read_len = esp_http_client_read(client, buffer, sizeof(buffer));
        if (read_len < 0) {
            ESP_LOGE(TAG, "Error reading data or connection closed prematurely");
            free(h_buf);
            esp_http_client_cleanup(client);
            return false;
        }
        if (read_len == 0) {
            break;
        }

        memcpy(h_buf + total_read, buffer, read_len);
        total_read += read_len;

        if (total_read % (1024 * 10) == 0) {
            ESP_LOGI(TAG, "Download Progress: %d / %d bytes", total_read, content_length);
        }
    }

    esp_http_client_cleanup(client);

    if (total_read == content_length) {
        uint16_t target_dir_id = get_or_create_dir("downloaded");
        const char *filename = get_filename_from_url(url);

        ESP_LOGI(TAG, "Saving file '%s' into '/downloaded/' folder (Dir ID: %d)...", filename, target_dir_id);

        if (fs_write_file(filename, h_buf, total_read, target_dir_id) < 0) {
            ESP_LOGE(TAG, "Failed to save file onto LFS partition! Disk may be full.");
            free(h_buf);
            return false;
        }

        free(h_buf);
        ESP_LOGI(TAG, "PXE Download 100%% complete!");
        return true;
    } else {
        ESP_LOGE(TAG, "Download incomplete! Got %d out of %d", total_read, content_length);
        free(h_buf);
        return false;
    }
}

// ─── 2. WIRELESS SYSTEM FIRMWARE SELF-UPDATE (Direct Hardware Streaming) ────

bool pxe_bios_ota_execute(const char* url) {
    ESP_LOGW(TAG, "Starting Network BIOS OTA from: %s", url);

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 5000,
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
    ESP_LOGI(TAG, "Erasing OTA partition...");

    esp_ota_handle_t update_handle = 0;
    err = esp_ota_begin(update_part, OTA_SIZE_UNKNOWN, &update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }

    // Stream and flash the partition in 1024-byte chunks (Consumes only 1 KB of RAM!)
    char buffer[1024];
    int total_read = 0;

    while (total_read < content_length) {
        int read_len = esp_http_client_read(client, buffer, sizeof(buffer));
        if (read_len < 0) {
            ESP_LOGE(TAG, "Error reading data or connection closed prematurely");
            esp_ota_abort(update_handle);
            esp_http_client_cleanup(client);
            return false;
        }
        if (read_len == 0) {
            break;
        }

        err = esp_ota_write(update_handle, buffer, read_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            esp_ota_abort(update_handle);
            esp_http_client_cleanup(client);
            return false;
        }

        total_read += read_len;

        if (total_read % (1024 * 50) == 0) {
            ESP_LOGI(TAG, "OTA Progress: %d / %d bytes", total_read, content_length);
        }
    }

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
    } else {
        ESP_LOGE(TAG, "OTA Download incomplete! Got %d out of %d", total_read, content_length);
        esp_ota_abort(update_handle);
        return false;
    }
}
