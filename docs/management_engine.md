# OpenC6 Management Engine (ME) & LP-Core Architecture

The OpenC6 Management Engine (ME) consists of coprocessor firmware (`lp_core_main.c`), a host-side manager (`management_engine.c`), and a shared memory segment (`me_shared.h`). Utilizing the ESP32-C6 low-power 32-bit RISC-V coprocessor (LP-Core), the ME operates in an isolated power domain. It acts as an autonomous out-of-band hardware supervisor, handling watchdogs, thermal boundaries, adaptive clock throttling, and advanced power button actions.

---

## 1. Inter-Processor Communication (IPC) via LP-RAM

Because the High-Performance (HP) core and the Low-Power (LP) core reside on the same silicon, they share access to a dedicated low-power memory segment (LP-SRAM). Variables declared in `lp_core_main.c` are compiled into this space and exported to the HP-core toolchain with a `ulp_` prefix (e.g., `me_cpu_load` becomes `ulp_me_cpu_load`).
```text
  ┌────────────────────────────────────────────────────────────┐
  │                 Shared LP-SRAM (IPC)                       │
  │                                                            │
  │  ulp_me_hp_is_awake  ◄─── [Status Flag] ───►  HP state     │
  │  ulp_me_wdt_counter  ◄─── [Heartbeat] ─────►  WDT feed     │
  │  ulp_me_cpu_load     ◄─── [CPU Load %] ────►  HP metrics   │
  │  ulp_me_target_freq  ◄─── [Target Freq] ───►  DVFS clock   │
  │  ulp_me_temperature  ◄─── [Junction Temp] ─►  TSENS read   │
  └────────────────────────────────────────────────────────────┘
```
These shared registers allow non-blocking, lock-free communication between both cores at a hardware level.

---

## 2. Boot Reasons & Diagnostic Matrix

When the system reboots, the BIOS host reads the shared variable `ulp_me_boot_reason` before any secondary initialization. Because LP-RAM contains garbage on cold (DC power loss) boots, the host validates the boot reason before processing it.

| Constant                     | Value | Trigger Condition                            | System Action                        |
| :--------------------------- | :---: | :------------------------------------------- | :----------------------------------- |
| `ME_BOOT_REASON_NONE`        |  `0`  | Default state or uninitialized LP-RAM        | Normal boot flow                     |
| `ME_BOOT_REASON_NORMAL`      |  `1`  | Short press of Power Button (< 3s)           | Standard wake up into OS / Payload   |
| `ME_BOOT_REASON_SETUP`       |  `2`  | Power Button held for 3 seconds              | Boot dispatcher opens Web Setup      |
| `ME_BOOT_REASON_FORCE_RESET` |  `3`  | Power Button held for 5 seconds              | Hard reset via `LP_WDT`              |
| `ME_BOOT_REASON_WDT`         |  `4`  | HP-core failed to pet WDT for 15s            | Hard reset and recovery boot         |
| `ME_BOOT_REASON_THERMAL`     |  `5`  | Core junction temperature >= Emergency Limit | Hard shutdown and return to S5 state |

## 3. LP-Core Coprocessor Implementation (`lp_core_main.c`)

The coprocessor runs a continuous loop with a baseline tick rate of 10 ms (`ulp_lp_core_delay_us(10000)`). It functions independently of whether the HP-core is active, sleeping, or locked up.

### 3.1 Intelligent Power Button Engine

The LP-core monitors physical sensory inputs connected to `PIN_BTN_SENSE` (GPIO 4, with pull-up) and `PIN_BTN_GND` (GPIO 3, driven output low).

* **Deep Sleep (S5 Soft-Off):** If the HP-core is asleep (`me_hp_is_awake == 0`), releasing the button after 50 ms triggers a normal boot (`ME_BOOT_REASON_NORMAL`). Releasing it after 3000 ms flags `ME_BOOT_REASON_SETUP` and wakes the HP-core.
* **Force Shutdown:** If the HP-core is awake and the power button is held for more than 5000 ms, the LP-core immediately triggers a physical system reset.

### 3.2 HP-Core Software Watchdog Monitor

While the HP-core is executing an OS or payload, the host must continuously "pet" the watchdog by calling `management_engine_pet_watchdog()`. This increments `ulp_me_wdt_counter`.

* Every 10 ms, the LP-core compares the current counter to the last saved value.
* If the value remains stagnant for 15,000 ms (`ME_WDT_TIMEOUT_MS`), the LP-core flags a deadlock state on the HP-core and invokes a hardware-level reset.

### 3.3 SchedUtil Dynamic Governor & Thermal Shield

Every 100 ms, the LP-core executes its performance and safety scaling algorithm:

                  [ Every 100ms: SchedUtil Loop ]
                                 │
                                 ▼
                     ┌───────────────────────┐
                     │ Temperature >= Limit? │
                     └───────────┬───────────┘
                                 │
                   ┌─────────────┴─────────────┐
                   ▼ Yes                       ▼ No
       ┌───────────────────────┐   ┌───────────────────────────┐
       │ Force Throttling      │   │ Calculate Load Target:    │
       │ Target Freq = 80 MHz  │   │  - Load > 75%: 160 MHz    │
       └───────────────────────┘   │  - Load > 30%: 120 MHz    │
                                   │  - Load <= 30%: 80 MHz    │
                                   └───────────┬───────────────┘
                                               │
                                               ▼
                                   ┌───────────────────────────┐
                                   │ Cap target at me_max_freq │
                                   │ Write to me_target_freq   │
                                   └───────────────────────────┘

1. **Emergency Thermal Shutdown:** If the junction temperature `me_temperature` exceeds `me_emergency_temp` (default: 75°C), the LP-core triggers a `ME_BOOT_REASON_THERMAL` hardware reset.
2. **Thermal Throttling:** If the temperature exceeds `me_throttle_temp` (default: 55°C), the governor locks the clock speed to 80 MHz to prevent thermal runaway.
3. **Load-Adaptive Governor:** Under normal thermal conditions, the clock is adjusted based on the load value `me_cpu_load` reported by the host:
   * **Load > 75%**: Scales to **160 MHz** (Maximum Performance)
   * **Load > 30%**: Scales to **120 MHz** (Balanced)
   * **Load <= 30%**: Scales to **80 MHz** (Power Saving)
4. **Limits Enforced:** The final computed target is capped by the maximum allowed frequency parameter (`me_max_freq`) fetched dynamically from NVRAM settings.

---

## 4. Hardware Reset Mechanism (`LP_WDT`)

To trigger a full chip hardware reset from the LP-core, the firmware accesses the RTC watchdog registers directly. This bypasses standard software reset vectors, ensuring a clean reboot even if the primary processor is in a hard fault or lockup state.

### Low-Level Register Key Exchange

To prevent accidental triggers, the LP watchdog registers are hardware-protected. The LP-core must write a specific security key to the write protection register before configuring the timer:

LP_WDT_WPROTECT = 0x50D83AA1UL; // Write Key unlock
LP_WDT_FEED = (1UL << 31);      // Feed Watchdog
LP_WDT_CONFIG1 = 100UL;         // Configure reset delay ticks

The LP-core then changes Stage 0 configuration of the watchdog to propagate a system-level RTC reset (LP_WDT_STG0_RTC_RST) and enables the watchdog. Once configured, the write-protection is re-enabled by writing 0 to LP_WDT_WPROTECT.
5. Host-Side Management Driver (management_engine.c)

The host driver abstracts the LP-core lifecycle, binary deployment, and health monitoring.
5.1 Initialization Strategy (Cold vs. Warm Boot)

During system startup, the host evaluates if the reboot reason was a deep sleep wakeup:

    Warm Wakeup Path (is_cold_boot == false): The LP-core is already running in LP-RAM and keeping track of system states. The host skips binary deployment to avoid resetting active timers.

    Cold Boot Path (is_cold_boot == true): The host loads the compiled binary (me_core_bin_start to me_core_bin_end) using ulp_lp_core_load_binary(), resets the IPC registers, and launches the LP-core.

5.2 Boot Reason Validation

To filter out random SRAM values present at power-on, the host validates the boot reason byte before reacting:

static bool is_valid_boot_reason(uint32_t r);

If the read byte does not match a valid boot reason enum, the host logs a warning and defaults to ME_BOOT_REASON_NONE.
