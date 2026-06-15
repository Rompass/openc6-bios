OpenC6 NVRAM Subsystem: Virtual CMOS & AI Tweaker Schema

The NVRAM module (nvram.c, nvram_defaults.c, and nvram_schema.h) acts as a
Virtual CMOS system for the ESP32-C6. It encapsulates hardware configurations,
tuning parameters, and network credentials. Utilizing the ESP-IDF Non-Volatile
Storage (NVS) flash partition beneath a dedicated namespace, this module handles
data corruption, implements safe fallback states, and exposes controls for
physical chip-level optimization under the AI Tweaker profile.

1. Namespace & Database Schema

All settings are isolated within the NVS namespace defined as BIOS_NVS_NAMESPACE
("bios_cfg").

To maximize flash endurance and read speeds, configuration parameters are stored
as compact primitive integers (uint8_t) or constrained strings.

### Comprehensive Key-Value Map

| NVS Key      | Data Type | Schema Enumeration / Constraints                                            | Default Value                             | Description                                         |
| :----------- | :-------: | :-------------------------------------------------------------------------- | :---------------------------------------- | :-------------------------------------------------- |
| `"dc_loss"`  | `uint8_t` | `0`: `DC_LOSS_POWER_OFF`; `1`: `DC_LOSS_POWER_ON`                           | `0`                                       | System power state policy upon DC power recovery    |
| `"post_led"` | `uint8_t` | `0`: `POST_LED_DISABLED`; `1`: `POST_LED_ENABLED`                           | `1`                                       | Enables/disables physical POST LED blinking codes   |
| `"wifi_ssid"`|  `string` | Max length: 32 bytes                                                        | `""` (Empty)                              | Local station SSID for internet connectivity        |
| `"wifi_pass"`|  `string` | Max length: 64 bytes                                                        | `""` (Empty)                              | Local station Wi-Fi WPA2 pre-shared key             |
| `"pxe_url"`  |  `string` | Max length: 128 bytes                                                       | `http://192.168.1.1:8080/payload.bin`     | Remote target path for PXE network booting          |
| `"aura_mode"`| `uint8_t` | `0`: `AURA_DISABLED`; `1`: `AURA_RAINBOW`                                   | `1`                                       | Controls the background Aura Sync animation         |
| `"aura_br"`  | `uint8_t` | `0` to `255` (Full range)                                                   | `128` (50%)                               | Scales maximum RMT LED brightness output            |
| `"cpu_freq"` | `uint8_t` | `80`: `CPU_FREQ_80MHZ`; `160`: `CPU_FREQ_160MHZ`                            | `160`                                     | Hardware limit configuration for the CPU clock      |
| `"cpu_gov"`  | `uint8_t` | `0`: `GOV_PERFORMANCE`; `1`: `GOV_DYNAMIC`                                  | `1` (DYNAMIC)                             | Dynamic Governor (SchedUtil) or Solid Power profiling|
| `"bod_lvl"`  | `uint8_t` | `0`: `BOD_STRICT` (2.8V); `1`: `BOD_RELAXED` (2.5V); `2`: `BOD_DISABLED`    | `0`                                       | Hardware Brownout Detector reset threshold          |
| `"me_state"` | `uint8_t` | `0`: `ME_DISABLED`; `1`: `ME_ENABLED`                                       | `1`                                       | Operating state of the LP-Core Management Engine    |
| `"temp_th"`  | `uint8_t` | Temperature in Celsius                                                      | `55`                                      | Threshold where thermal clock throttling initiates  |
| `"temp_em"`  | `uint8_t` | Temperature in Celsius                                                      | `75`                                      | Threshold where thermal emergency shutdown triggers |
| `"ota_pend"` | `uint8_t` | `0`: `BIOS_UPDATE_NONE`; `1`: `BIOS_UPDATE_PENDING`                         | `0`                                       | Flag to trigger network BIOS OTA upon boot          |         |

2. Clear CMOS (Factory Reset Reset Routine)

The module implements a complete "Clear CMOS" routine inside
nvram_load_defaults(). This function can be triggered via the local
configuration Web UI or by shorting the hardware jumper PIN_CLEAR_NVRAM (GPIO 2)
to PIN_CMOS_GND (GPIO 1) during boot.

                  [ Jumper / Setup Reset Signal ]
                                 │
                                 ▼
                     ┌───────────────────────┐
                     │ Erase NVS Flash Space │
                     └───────────┬───────────┘
                                 │
                                 ▼
                     ┌───────────────────────┐
                     │  Re-initialize NVS    │
                     └───────────┬───────────┘
                                 │
                                 ▼
                     ┌───────────────────────┐
                     │ Write Safe Enums to   │
                     │ All Schema Parameters │
                     └───────────────────────┘

1.  Sector Wipe: It wipes the physical NVS flash sector completely via
    nvs_flash_erase(). This erases all active keys, namespaces, and
    wear-leveling markers, ensuring no stale settings survive.
2.  Re-initialization: Re-creates the virtual sector partition table using
    nvram_init().
3.  Provisioning: Writes the safe default values (outlined in the key-value map)
    to each active configuration slot and commits changes to persistent flash
    storage.

3. AI Tweaker: Advanced Hardware Tuning Controls

The AI Tweaker profile permits lower-level customization of the ESP32-C6 silicon
properties.

3.1 CPU Frequency and Governor Profiles

  - Clock Limits (cpu_freq_t): Allows locking the maximum execution clock
    boundary. Selecting CPU_FREQ_80MHZ reduces dynamic power consumption, while
    CPU_FREQ_160MHZ unlocks peak RISC-V compute performance.
  - Power Scaling Governors (cpu_governor_t):
      - GOV_PERFORMANCE: Locks the CPU frequency to its highest allowed limit
        continuously. Reduces clock switching overhead for time-sensitive
        payloads.
      - GOV_DYNAMIC: Integrates with the Management Engine (ME) and the idle
        task. It dynamically scales the clock (80, 120, or 160 MHz) every 100 ms
        based on active CPU load, reducing thermal dissipation.

3.2 Brownout Detector (BOD) Protection Levels (bod_level_t)

The BOD prevents processor misbehavior caused by transient voltage drops under
heavy transmission loads.

  - BOD_STRICT (2.8V): Triggers a clean hardware reset as soon as the main power
    rail drops below 2.8V. Recommended for maximum stability on low-cost USB
    power blocks.
  - BOD_RELAXED (2.5V): Lowers the threshold to 2.5V, preventing unnecessary
    resets under minor power surges.
  - BOD_DISABLED: Disables hardware voltage drop detection entirely. Highly
    discouraged, as it can cause instruction corruption on weak power networks.

3.3 Dynamic Thermal Limits

Allows custom tuning of safety thresholds used by the Management Engine thermal
monitoring loop:

  - Throttling Temperature (temp_th): If the junction temperature matches this
    value, the SchedUtil governor locks the clock to 80 MHz to allow passive
    cooling.
  - Emergency Temperature (temp_em): If the junction temperature climbs to this
    threshold, the LP-core immediately issues a hard shutdown to protect the
    silicon.

4. API Reference

5.1 Storage Lifecycle Management

esp_err_t nvram_init(void);

Initializes the underlying NVS Flash driver. Automatically erases and
re-partitions NVS sectors if flash page allocation limits or format version
incompatibilities are detected.

esp_err_t nvram_load_defaults(void);

Clears the active configuration database entirely and populates it with baseline
safe factory defaults.

5.2 NVS Abstracted Getters and Setters

Getter and setter wrappers handle NVS space formatting, parameter casting, and
error recovery:

  - Wi-Fi Config: nvram_get_wifi_sta_config(...) reads string credentials
    directly into separate destination buffers.
  - PXE URL: nvram_get_pxe_url(...) reads the network boot path. If the key is
    missing from flash, it automatically falls back to the safe macro constant
    VAL_PXE_URL_DEFAULT.
  - AI Tweaker Getters/Setters: Provide safe casting between arbitrary integer
    types stored in NVS and the typed schema enums (cpu_freq_t, cpu_governor_t,
    bod_level_t, me_state_t).
