#pragma once
#include <stdbool.h>
#include <stdint.h>

void     management_engine_init(bool is_enabled, bool is_cold_boot);
bool     management_engine_is_active(void);

bool     management_engine_check_force_shutdown(void);
uint32_t management_engine_get_boot_reason(void);
void     management_engine_clear_boot_reason(void);

void     management_engine_pet_watchdog(void);
