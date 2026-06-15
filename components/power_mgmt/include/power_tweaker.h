#pragma once

/**
 * @brief Reads AI Tweaker configurations from NVRAM and applies them to the SoC.
 * Configures the active core frequency limits, PMU profiles, and BOD state.
 */
void power_tweaker_apply_bios_settings(void);
