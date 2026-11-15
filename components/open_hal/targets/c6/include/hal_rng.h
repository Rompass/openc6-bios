#pragma once

#include <stdint.h>
#include "soc/soc.h"
#include "soc/wdev_reg.h"

/**
 * @brief Reads hardware true random 32-bit value from ESP32-C6 silicon TRNG.
 */
static inline uint32_t hal_rng_get_u32(void)
{
    return REG_READ(WDEV_RND_REG);
}
