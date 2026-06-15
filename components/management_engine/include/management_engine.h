#pragma once
#include <stdbool.h>
#include <stdint.h>

// is_cold_boot=true  -> Reload and deploy the LP core firmware binary from scratch (cold boot)
// is_cold_boot=false -> LP core is already running, preserve states and bypass deployment (warm wakeup)
void management_engine_init(bool is_enabled, bool is_cold_boot);

bool     management_engine_check_force_shutdown(void);
uint32_t management_engine_get_boot_reason(void);
void     management_engine_clear_boot_reason(void);

// Feeds the LP Core Software Watchdog. Invoke every ~2s while the HP Core is active.
// If feeding halts for >= 15s, LP Core triggers a hardware system reset and logs
// ME_BOOT_REASON_WDT in shared memory. Has no effect if the Management Engine is disabled.
void management_engine_pet_watchdog(void);
