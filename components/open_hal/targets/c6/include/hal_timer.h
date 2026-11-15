#pragma once

#include <stdint.h>
#include "esp_timer.h"
#include "rom/ets_sys.h"

static inline uint64_t hal_timer_get_us(void)
{
    return (uint64_t)esp_timer_get_time();
}

static inline uint32_t hal_timer_get_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static inline __attribute__((always_inline)) void hal_timer_delay_us(uint32_t us)
{
    ets_delay_us(us);
}
