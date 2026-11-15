#pragma once

#include "openc6_abi.h"
#include "hal_board.h"

void bios_core_start(void);
void bios_enter_s5_state(void);

const openc6_abi_t *bios_get_abi(void);
