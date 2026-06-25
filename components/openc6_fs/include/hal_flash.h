#ifndef HAL_FLASH_H
#define HAL_FLASH_H

#include <stdint.h>
#include <stddef.h>

#define FLASH_SIZE   (1024 * 1024)

void flash_read(uint32_t addr, uint8_t *buf, size_t size);
void flash_write(uint32_t addr, const uint8_t *buf, size_t size);
void flash_erase_sector(uint32_t sector_num);
void hal_flash_init(void);
uint32_t hal_flash_get_size(void);

#endif
