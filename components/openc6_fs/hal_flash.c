#include "hal_flash.h"
#include "openc6_fs.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "hw_usb.h"

static const esp_partition_t *s_fs_partition = NULL;
static const char *TAG = "HAL_FLASH";

/**
 * @brief Locates and maps the dedicated OpenC6 Flash filesystem partition.
 */
void hal_flash_init(void) {
    s_fs_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0xff, "openc6_fs");

    if (!s_fs_partition) {
        s_fs_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "openc6_fs");
    }

    if (!s_fs_partition) {
        ESP_LOGE(TAG, "Partition 'openc6_fs' not found! Check your partitions.csv");
    } else {
        /* Log physical partition boundary mapping */
        ESP_LOGI(TAG, "Partition 'openc6_fs' located: 0x%08lX, Size: %lu KB (%lu sectors)",
                 (unsigned long)s_fs_partition->address,
                 (unsigned long)(s_fs_partition->size / 1024),
                 (unsigned long)(s_fs_partition->size / SECTOR_SIZE));
    }
}

/**
 * @brief Writes data buffer directly into SPI Flash partition offset.
 */
void flash_write(uint32_t addr, const uint8_t *buf, size_t size) {
    if (!s_fs_partition) return;
    esp_partition_write(s_fs_partition, addr, buf, size);
}

/**
 * @brief Reads data buffer directly from SPI Flash partition offset.
 */
void flash_read(uint32_t addr, uint8_t *buf, size_t size) {
    if (!s_fs_partition) return;
    esp_partition_read(s_fs_partition, addr, buf, size);
}

/**
 * @brief Erases a single 4096-byte sector within the mapped partition boundary.
 * Captures and logs hardware partition errors directly to bare-metal USB registers.
 *
 * @param sector_num Zero-based sector index inside the mapped partition.
 */
void flash_erase_sector(uint32_t sector_num) {
    if (!s_fs_partition) {
        hw_usb_print("[HAL_ERR] s_fs_partition is NULL!\r\n");
        return;
    }
    esp_err_t err = esp_partition_erase_range(s_fs_partition, sector_num * SECTOR_SIZE, SECTOR_SIZE);
    if (err != ESP_OK) {
        dbg_usb_printf("[HAL_ERR] Sector %lu erase failed: 0x%x\r\n",
                       (unsigned long)sector_num, (unsigned int)err);
    }
}

/**
 * @brief Returns total storage capacity allocated for the filesystem in bytes.
 */
uint32_t hal_flash_get_size(void) {
    return s_fs_partition ? s_fs_partition->size : FLASH_SIZE;
}
