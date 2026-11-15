#pragma once

#include <stdint.h>

/**
 * @brief Configures hardware RISC-V PMP isolation window around the sandbox arena.
 *
 * @param arena_start Start physical address of the U-mode RWX arena.
 * @param arena_end   End physical address of the U-mode RWX arena.
 */
void hal_pmp_setup(uintptr_t arena_start, uintptr_t arena_end);

/**
 * @brief Disables and clears all sandbox PMP region entries.
 */
static inline void hal_pmp_clear(void)
{
    hal_pmp_setup(0, 0);
}
