#include "hal_flash.h"
#include "openc6_fs.h"
#include "esp_partition.h"
#include "esp_log.h"

static const esp_partition_t *s_fs_partition = NULL;
static const char *TAG = "HAL_FLASH";

void hal_flash_init(void) {
    s_fs_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0xff, "openc6_fs");

    if (!s_fs_partition) {
        s_fs_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "openc6_fs");
    }

    if (!s_fs_partition) {
        ESP_LOGE(TAG, "Partition 'openc6_fs' not found! Check your partitions.csv");
    } else {
        ESP_LOGI(TAG, "OpenC6 FS mapped successfully!");
        ESP_LOGI(TAG, "Address: 0x%08lX, Size: %lu KB (%lu sectors)",
                 (unsigned long)s_fs_partition->address,
                 (unsigned long)(s_fs_partition->size / 1024),
                 (unsigned long)(s_fs_partition->size / SECTOR_SIZE));
    }
}

void flash_write(uint32_t addr, const uint8_t *buf, size_t size) {
    if (!s_fs_partition) return;
    esp_partition_write(s_fs_partition, addr, buf, size);
}

void flash_read(uint32_t addr, uint8_t *buf, size_t size) {
    if (!s_fs_partition) return;
    esp_partition_read(s_fs_partition, addr, buf, size);
}

void flash_erase_sector(uint32_t sector_num) {
    if (!s_fs_partition) return;
    esp_partition_erase_range(s_fs_partition, sector_num * SECTOR_SIZE, SECTOR_SIZE);
}

uint32_t hal_flash_get_size(void) {
    return s_fs_partition ? s_fs_partition->size : FLASH_SIZE;
}
