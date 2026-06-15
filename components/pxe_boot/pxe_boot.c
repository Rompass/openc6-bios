#include "pxe_boot.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_partition.h"
#include "esp_ota_ops.h" // Added hardware OTA update driver support

static const char *TAG = "PXE_BOOT";

// ─── 1. USER PAYLOAD NETWORK DEPLOYMENT (XIP TARGET) ─────────────────────────

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

    ESP_LOGI(TAG, "Found payload. Size: %d bytes. Preparing Flash...", content_length);

    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0xff, "network_buf");
    if (!part || content_length > part->size) {
        ESP_LOGE(TAG, "Flash partition 'network_buf' not found or too small");
        esp_http_client_cleanup(client);
        return false;
    }

    // Erase the required flash range (size must be aligned to a 4KB sector boundary)
    uint32_t erase_size = (content_length + 4095) & ~4095;
    esp_partition_erase_range(part, 0, erase_size);
    ESP_LOGI(TAG, "Flash erased. Downloading payload...");

    // Fetch and commit payload stream in 1024-byte chunks
    int total_read = 0;
    char buffer[1024];

    while (total_read < content_length) {
        int read_len = esp_http_client_read(client, buffer, sizeof(buffer));
        if (read_len < 0) {
            ESP_LOGE(TAG, "Error reading data or connection closed prematurely");
            esp_http_client_cleanup(client);
            return false;
        }
        if (read_len == 0) {
            break; // End of file stream reached
        }

        esp_partition_write(part, total_read, buffer, read_len);
        total_read += read_len;

        // Print progress telemetry every ~10 KB of data transfer
        if (total_read % (1024 * 10) == 0) {
            ESP_LOGI(TAG, "Progress: %d / %d bytes", total_read, content_length);
        }
    }

    esp_http_client_cleanup(client);

    if (total_read == content_length) {
        ESP_LOGI(TAG, "PXE Download 100%% complete! (%d bytes written to Flash)", total_read);
        return true;
    } else {
        ESP_LOGE(TAG, "Download incomplete! Got %d out of %d", total_read, content_length);
        return false;
    }
}

// ─── 2. WIRELESS SYSTEM FIRMWARE SELF-UPDATE (BIOS OTA) ──────────────────────

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

    // Automatically locate the passive/inactive system boot OTA partition slot
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

    // Fetch and write firmware data in 1024-byte block sequences
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
            break; // End of file stream reached
        }

        err = esp_ota_write(update_handle, buffer, read_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            esp_ota_abort(update_handle);
            esp_http_client_cleanup(client);
            return false;
        }

        total_read += read_len;

        // Print progress telemetry every ~50 KB to avoid flooding slow UART terminals
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

        // Re-target the second-stage bootloader vector flags to run from the newly written partition
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
