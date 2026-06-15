#pragma once

// --- Power Button GPIO Pins (Managed by Management Engine) ---
#define PIN_PWR_BTN_GND  3   // Software Ground for power button
#define PIN_PWR_BTN      4   // Power button sensor pin (Active Low)

// --- Service / Utility GPIO Pins ---
#define PIN_CMOS_GND     1   // Software Ground for Clear CMOS jumper
#define PIN_CLEAR_NVRAM  2   // Clear CMOS jumper sensor pin (Active Low)
#define PIN_POST_LED     8   // Onboard POST LED (Common on dev boards)

void bios_core_start(void);
void bios_enter_s5_state(void);
