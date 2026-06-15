# OpenC6 LED Management: Diagnostics & Aura Sync

The LED Management module (`led_mgmt.c` and its associated header) controls the onboard addressable RGB status LED. Utilizing the ESP32-C6 Remote Control (RMT) peripheral, this module handles low-level hardware diagnostics during the Power-On Self-Test (POST) phase, maps color space conversions, and operates an autonomous background thread for dynamic lighting animations.

---

## 1. Hardware and Peripheral Integration

The module abstracts an addressable RGB pixel connected to the physical pin defined by `PIN_POST_LED` (GPIO 8).

* **Driver Subsystem:** It interfaces with the ESP-IDF `led_strip` component, utilizing the hardware RMT transceiver configured at a timing resolution of **10 MHz**.
* **Color Channel Re-routing:** The physical LED utilizes a GBR (Green, Blue, Red) channel layout instead of standard RGB. The driver internally translates arguments to prevent color inversion:

  led_strip_set_pixel(led_strip, 0, green_value, blue_value, red_value);

2. Design Architecture & Core Features
2.1 HSV to RGB Color Space Conversion

To achieve smooth, natural color transitions (such as the rainbow effect), the module implements an HSV (Hue, Saturation, Value) to RGB conversion helper.

    Hue (H): Controls the color angle (0° to 359°).

    Saturation (S): Fixed at maximum (255) to maintain deep color saturation.

    Value (V): Directly mapped to the system-wide brightness configuration in NVRAM (0 to 255), allowing linear dimming of running animations without color shifting.

2.2 Exclusive LED Control Locking

To prevent race conditions and visual flickering between different system tasks, the module enforces a priority-based control lock:
code Code
```text
[ LED State Modification Request ]
                              │
                              ▼
                 ┌──────────────────────────┐
                 │  Is Aura Task Running?   │
                 └────────────┬─────────────┘
                              │
                ┌─────────────┴─────────────┐
                ▼ Yes                       ▼ No
   ┌────────────────────────┐   ┌────────────────────────┐
   │ Ignore Static Color    │   │ Scale color brightness │
   │ Override (Lock Active) │   │ Apply GBR re-routing   │
   └────────────────────────┘   │ Write to RMT hardware  │
                                └────────────────────────┘
```
    Aura Active Lock: When the background animation task (aura_effect_task) is active, static color updates requested via led_mgmt_set_color() are discarded.

    Lock Bypass: Diagnostic POST blinks (led_mgmt_blink_post) bypass this lock. They temporarily disable the Aura animation, perform the requested flash sequence, and clear the strip to ensure system diagnostics are highly visible.

3. Background Animation: Aura Sync (Rainbow Flow)

When set to AURA_RAINBOW mode, the module spawns a dedicated FreeRTOS task running at priority 5:

static void aura_effect_task(void *arg);

    Dynamic Brightness Scaling: The task queries the NVRAM brightness setting during every cycle. Adjustments made in the BIOS Web UI are applied in real-time.

    Performance Impact: The task relies on standard non-blocking delays (vTaskDelay) with a sleep interval of 30 ms, minimizing CPU utilization on the high-performance core.

4. Constant Definitions (Default Color Maps)

The header file defines stable color combinations representing the system's operational states:
Constant	Red Channel	Green Channel	Blue Channel	Visual Indication
COLOR_POST_OK	0	255	0	Solid Green: Hardware checks passed; booting payload
COLOR_POST_ERROR	255	0	0	Solid Red: Critical hardware failure / emergency halts
COLOR_BIOS_SETUP	0	0	255	Solid Blue: Local Web Setup (AP Mode) active

5. API Reference
5.1 System Initializer

void led_mgmt_init(void);

Initializes the physical RMT transceiver channel, binds the GPIO line, allocates system buffers, and clears the pixel state to dark.
5.2 Static Color Controller

void led_mgmt_set_color(uint8_t r, uint8_t g, uint8_t b);

Updates the LED to a solid static color.

    Scales input channels (R, G, B) relative to the user-defined brightness threshold in NVRAM.

    Re-orders byte sequences to match the physical GBR silicon layout.

    This call is ignored if the background Aura animation thread is active.

5.3 Diagnostic POST Blinker

void led_mgmt_blink_post(uint8_t r, uint8_t g, uint8_t b, int count);

Performs highly visible flashing sequences during system boot.

    Queries NVRAM to verify if POST LED indicators are enabled (POST_LED_ENABLED).

    Runs a synchronous flashing sequence (100 ms active, 100 ms inactive).

    Automatically bypasses background tasks to ensure critical alert sequences are never overridden.

5.4 Aura Mode Selector

void led_mgmt_set_aura_mode(aura_mode_t mode);

Controls background animations.

    AURA_RAINBOW: Spawns aura_effect_task if it is not already running.

    AURA_DISABLED: Terminates the background task, releases its stack allocation, and powers down the LED strip.
