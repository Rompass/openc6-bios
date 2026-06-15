# OpenC6 Power Management: AI Tweaker, BOD, and SchedUtil Governor

The Power Management system (`power_tweaker.c`, `power_governor.c`, and `power_tweaker.h`) regulates the SoC's electrical and clock parameters. Configured via the BIOS AI Tweaker profile, it bridges low-level hardware register configurations with real-time software scheduling. It governs the hardware Brownout Detector (BOD), monitors core junction temperatures, and operates a high-priority dynamic SchedUtil-style clock governor.

---

## 1. Hardware Brownout Detector (BOD) Regulation

To protect metadata and prevent memory corruption during voltage drops under radio load or weak USB power adapters, the BIOS implements direct hardware-level BOD configuration.

The module interfaces with the hardware register space (`LP_AON_BROWN_OUT_REG`) via the ESP-IDF private brownout driver and Hardware Abstraction Layer (`hal/brownout_hal.h`):

                       [ DC Power Rail Drop ]
                                  │
                                  ▼
                    ┌───────────────────────────┐
                    │  Voltage below Threshold? │
                    └─────────────┬─────────────┘
                                  │
                                  ▼ Yes
                    ┌───────────────────────────┐
                    │   BOD Safety Mitigation:  │
                    │   - Shutdown RF Radios    │
                    │   - Power Down SPI Flash  │
                    │   - Issue SoC Hard Reset  │
                    └───────────────────────────┘

When an undervoltage condition occurs, the BOD halts instruction execution and executes safety measures before the CPU can enter an undefined, glitching state:

* **System Reset (`reset_enabled = true`):** Asserts a physical reset on the SoC.
* **Flash Protection (`flash_power_down = true`):** Power-downs the flash memory instantly during drops, ensuring unfinished write transactions do not corrupt partition tables or file system metadata.
* **RF Power-Shedding (`rf_power_down = true`):** Immediately cuts power to internal radio circuits, reducing instantaneous load on the power rail.

### BOD Threshold Configurations:

| BOD Mode       | Hal Threshold Value | Nominal Voltage | System Target                                                                                                               |
| :------------- | :-----------------: | :-------------: | :-------------------------------------------------------------------------------------------------------------------------- |
| `BOD_STRICT`   |         `7`         |     `~2.8V`     | **Maximum Security:** Halts the system at the earliest sign of dropouts. Recommended for stable flash writes.               |
| `BOD_RELAXED`  |         `4`         |     `~2.5V`     | **Tolerant Execution:** Allows deeper battery discharge and tolerates transient drops during high-power Wi-Fi transmission. |
| `BOD_DISABLED` |        `N/A`        |     `None`      | **Extreme Profile:** Disables physical undervoltage protection completely. Operating risk is handled entirely by the user.  |

---

## 2. Core Junction Temperature Sensor (TSENS)

Core temperature monitoring is managed globally by the background `openc6_temperature_task` polling at 2-second intervals.

* **Two-Stage Calibration:** Since integrated silicon sensors can vary during manufacturing, the driver uses a fallback calibration strategy:
  1. **High Range Configuration:** Attempts calibration for high operating temperatures (**20°C to 100°C**) via the default configuration macro.
  2. **Low Range Fallback:** If initial calibration fails, it configures a narrower operating window (**-10°C to 80°C**) to guarantee sensor initialization.
* **Coprocessor Communication:** Valid Celsius readings are written directly to the shared LP-SRAM location `ulp_me_temperature`.

---

## 3. Dynamic SchedUtil-Style Governor

When set to `GOV_DYNAMIC` mode, the system spawns the `openc6_schedutil_task` running at peak priority (`configMAX_PRIORITIES - 1`). This task operates on a 100 ms loop to recalculate system load and adjust the clock dynamically.

### 3.1 Real-Time CPU Load Calculation

The governor monitors CPU idle cycles dynamically. Because there is no raw kernel-level load counter, the task queries the cumulative execution time of the FreeRTOS Idle Task.

The active load percentage is calculated on every 100ms cycle using simple microsecond delta operations:

Delta_Total = Time_Now - Time_Last_Total
Delta_Idle  = Idle_Now - Idle_Last_Idle

CPU_Load (%) = 100 * (1 - (Delta_Idle / Delta_Total))

This approach provides a reliable load metric (from 0% to 100%) without introducing scheduling overhead.
3.2 Dual Operating Governors: Active ME vs. Legacy Fallback

The calculated CPU load is dispatched to one of two governor routing paths:
code Code
```text
[ Load Calculated ]
                               │
                 ┌─────────────┴─────────────┐
                 ▼ Yes                       ▼ No
   ┌────────────────────────┐   ┌────────────────────────┐
   │   Management Engine    │   │  Legacy Mode Fallback  │
   │                        │   │                        │
   │ 1. Write load to       │   │ 1. Evaluate Load:      │
   │    ulp_me_cpu_load     │   │     - >75%: 160 MHz    │
   │ 2. Fetch computed target│  │     - >30%: 120 MHz    │
   │    from me_target_freq │   │     - <=30%: 80 MHz    │
   │                        │   │ 2. Cap limit at NVS    │
   │                        │   │    max_bios_freq value │
   └───────────┬────────────┘   └───────────┬────────────┘
               │                            │
               └─────────────┬──────────────┘
                             ▼
                 ┌───────────────────────┐
                 │   Frequency Change?   │
                 └───────────┬───────────┘
                             │
                             ▼ Yes
                 ┌───────────────────────┐
                 │ Re-configure Hardware │
                 │  PMU Clock Frequency  │
                 └───────────────────────┘
```
Path A: Management Engine (ME) Active

    The HP-core writes the calculated load to ulp_me_cpu_load.

    The LP-core coprocessor executes the SchedUtil scaling calculations in its isolated thread (enforcing thermal limits).

    The HP-core reads the final targeted frequency from ulp_me_target_freq and applies the change.

Path B: Legacy Fallback Mode (ME Disabled)

    The HP-core performs the load-adaptive frequency mapping locally:

        Load > 75%: Targets 160 MHz

        Load > 30%: Targets 120 MHz

        Load <= 30%: Targets 80 MHz

    The target is clamped by the static upper frequency limit (max_bios_freq) fetched from NVRAM.

3.3 Frequency Transitions

To reduce transition latency, the governor only reconfigures the Power Management Unit (PMU) when the target frequency crosses a threshold:
```text
esp_pm_config_t pm_cfg = {
    .max_freq_mhz = target_freq,
    .min_freq_mhz = target_freq,
    .light_sleep_enable = false
};
esp_pm_configure(&pm_cfg);
```
Changing these parameters triggers clock-tree modulation on the ESP32-C6, modifying core voltage and clock frequencies transparently under active workloads.
4. API Reference
4.1 System Settings Applier
```text
void power_tweaker_apply_bios_settings(void);
```
Reads physical limits, power profiles, and BOD settings from NVRAM, configures the PMU, initializes BOD register levels, and launches thermal and governor tasks.
4.2 Governor Subsystem Start
```text
void power_governor_start(void);
```
Launches the background temperature monitoring loop. If the Management Engine is enabled, it writes thermal parameters (temp_th, temp_em) and performance boundaries to the LP-RAM segment, and spawns the schedutil governor task.
