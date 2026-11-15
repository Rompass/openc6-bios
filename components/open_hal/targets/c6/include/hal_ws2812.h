#pragma once

#include <stdint.h>

void hal_ws2812_init(void);
void hal_ws2812_unlock(void);
void hal_ws2812_write_rgb(uint8_t r, uint8_t g, uint8_t b);
void hal_ws2812_latch_low(void);
