#pragma once

#include "openc6_abi.h"

/* Power button GPIO definitions (managed by Management Engine) */
#define PIN_PWR_BTN_GND  3
#define PIN_PWR_BTN      4

/* Hardware service jumpers */
#define PIN_CMOS_GND     1
#define PIN_CLEAR_NVRAM  2
#define PIN_POST_LED     8

void bios_core_start(void);
void bios_enter_s5_state(void);

/**
 * @brief Returns a pointer to the centralized BIOS ABI jump-table.
 *
 * @return const openc6_abi_t* Global ABI instance.
 */
const openc6_abi_t *bios_get_abi(void);
