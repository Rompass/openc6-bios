# ESP32-C3 SuperMini: Technical Reference

OpenC6 BIOS builds for either ESP32-C6 (primary target) or ESP32-C3 from the
same codebase, selected via `CONFIG_IDF_TARGET`. This document covers what's
actually different about the C3 build and why. For step-by-step build/flash
instructions, see [QUICKSTART-ESP32-C3.md](QUICKSTART-ESP32-C3.md).

## Hardware differences

| Aspect | ESP32-C6 | ESP32-C3 SuperMini |
|--------|----------|---------------------|
| Flash | 8 MB | 4 MB (typical) |
| PSRAM | Optional, up to 16 MB | None |
| LP-Core coprocessor | Yes (Management Engine runs here) | **No** |
| EXT1/RTC-IO wakeup | Yes | **No** (plain GPIO wakeup only) |
| WiFi | 802.11ax (WiFi 6) | 802.11b/g/n (WiFi 4) |
| Max CPU clock | 160 MHz (dual dispatch) | 160 MHz (single core) |
| GPIO count | 32 | 22 |

## Pin mapping

Power button, clear-CMOS, BOOT, and POST LED pins are identical on both
targets (GPIO 8 works fine as a general-purpose pin on C3 too). The serial
payload loader runs over the board's native USB (no external UART adapter
or dedicated pins on either target). Only the AP SSID differs:

| Function | C6 | C3 | Why it moved |
|---|---|---|---|
| POST LED | GPIO 8 | GPIO 8 | unchanged |
| Power button / GND | GPIO 4 / 3 | GPIO 4 / 3 | unchanged |
| Clear NVRAM / GND | GPIO 2 / 1 | GPIO 2 / 1 | unchanged |
| BOOT button | GPIO 9 | GPIO 9 | unchanged |
| WiFi Setup AP SSID | `BIOS_SETUP_C6` | `BIOS_SETUP_C3` | cosmetic, so you can tell targets apart over WiFi |

SSID selection is done at compile time via `#ifdef CONFIG_IDF_TARGET_ESP32C3` — no runtime detection needed.

## What had to change in code

### 1. GPIO pins / SSID (`components/bios_core/include/bios_core.h`, `components/bios_core/bios_init.c`)
`PIN_POST_LED` is unconditional (GPIO 8 on both targets). The AP SSID uses a conditional `#define` guarded by `CONFIG_IDF_TARGET_ESP32C3`.

### 2. Management Engine / LP-Core (`components/management_engine/`)
C3 has no LP-Core, so the ULP binary can't be built or deployed there:
- `CMakeLists.txt` skips `ulp_embed_binary()` when `CONFIG_IDF_TARGET_ESP32C3` is set.
- `management_engine.c` provides plain HP-Core-backed storage for the `ulp_me_*` shared variables (normally auto-exported by the ULP linker) so the rest of the codebase still links.
- `management_engine_init()` unconditionally logs "Legacy Mode" and returns on C3 — the Management Engine simply doesn't run there; watchdog/thermal monitoring is not available on this target.

### 3. Sleep/wakeup (`components/bios_core/bios_init.c`)
C3 has no `SOC_ULP_SUPPORTED` and no `SOC_PM_SUPPORT_EXT1_WAKEUP` (no RTC-IO domain). The S5 power-off path and the post-wakeup detection are both guarded:
- `esp_sleep_enable_ulp_wakeup()` only compiles under `#if SOC_ULP_SUPPORTED`.
- EXT1 wakeup (`esp_sleep_enable_ext1_wakeup` + `ESP_EXT1_WAKEUP_ANY_LOW`) falls back to plain `gpio_wakeup_enable()` + `esp_sleep_enable_gpio_wakeup()` under `#if !SOC_PM_SUPPORT_EXT1_WAKEUP`.
- The wakeup-cause check on boot uses `ESP_SLEEP_WAKEUP_GPIO` instead of `ESP_SLEEP_WAKEUP_EXT1` on C3.

### 4. Partition table (`partitions_c3_4mb.csv`)
A dedicated 4 MB layout (C6 keeps using `partitions.csv`, 8 MB). App partitions must be 64 KB-aligned; sized to comfortably fit the ~1.1 MB firmware image with headroom:

```
nvs,         data, nvs,     0x9000,   0x5000
otadata,     data, ota,     0xe000,   0x2000
phy_init,    data, phy,     0x10000,  0x1000
bios_fw,     app,  factory, 0x20000,  0x120000
ota_0,       app,  ota_0,   0x140000, 0x120000
openc6_fs,   data, 0xff,    0x260000, 0x120000
payload_xip, data, 0x20,    0x380000, 0x80000
```

### 5. sdkconfig
`CONFIG_IDF_TARGET`, flash size (4 MB vs 8 MB), and the partition table path all need to point at the C3 variants — see the Quick Start guide for the exact commands.

## Power delivery (important — this bit the initial bring-up)

**ESP32-C3 SuperMini boards draw a real current spike** when the WiFi radio
initializes and again when NVS does its first flash-erase. A laptop USB port
(especially through a hub, or with a thin/marginal cable) can sag below the
brownout threshold under that load.

Symptoms observed during bring-up: the board would boot, print BIOS init
logs, then reset (`esp_reset_reason()` = `ESP_RST_BROWNOUT` /
`ESP_RST_PWR_GLITCH`) right as NVRAM/WiFi init started, and the USB-CDC port
would drop and reappear on a ~4 second cycle.

**Brownout detection is left enabled** (`CONFIG_ESP_BROWNOUT_DET=y`, level 7 —
2.51V, the lowest/most tolerant threshold available) because a clean reset is
safer than a silent hang if the supply sags. Disabling it as a one-off
diagnostic confirmed the chip does survive slightly longer without it, but
then hangs (no clean recovery) instead — it does not fix the underlying sag,
it just changes the failure mode from "reset" to "wedged."

**Fix: power the board from a USB wall charger or powered hub, not directly
off a laptop port.** This is a hardware power-delivery limit, not a firmware
bug — no `sdkconfig` setting resolves it.

## `BIOS_C3_ALWAYS_SETUP` (removed)

During initial C3 bring-up, `components/bios_core/CMakeLists.txt` briefly
defined `BIOS_C3_ALWAYS_SETUP`, making every cold boot skip straight to BIOS
Setup with no button presses. **This has been removed** — it caused a real
problem: a Setup UI "Save & Exit" triggers `esp_restart()` (a software reset),
which `bios_core_start()` still classifies as `is_cold_boot = true`, so the
always-Setup logic intercepted the reboot and ignored whatever the user had
just configured (e.g. Network Boot). Normal boot flow — respecting NVRAM's
`DC_LOSS_ACTION` and the BOOT-button interactive menu — is restored.

## Known limitation: native USB registers (unverified on C3)

Since the HAL refactor, the BIOS console and serial payload loader use direct
USB-Serial-JTAG register access in `components/openc6_hal/include/hw_usb.h`,
with addresses taken from the ESP32-C6 TRM (USB-Serial-JTAG at `0x6000F000`,
clock/reset via the C6-only PCR block at `0x6009608C`). The C3 has a different
register map for both, so `hw_usb.h` has not been made target-specific and
**the C3 build has not been re-validated since the merge with the refactor**. The checklist below predates it.

## Testing checklist

- [x] Builds cleanly for `esp32c3` target
- [x] Flashes over USB-CDC (`/dev/cu.usbmodem*`)
- [x] Boots past NVRAM/WiFi init without brownout reset (steady power required)
- [x] `BIOS_SETUP_C3` AP appears, connects with password `12345678`
- [x] Web UI reachable at `http://192.168.4.1`
- [x] WiFi STA connects to a WPA2-PSK home network
- [x] Network boot (PXE): downloads `payload.bin` over HTTP, saves to `openc6_fs`
- [x] RAM payload execution: BIOS jumps to downloaded payload, ABI calls (malloc, SHA-256, TRNG, math, WiFi, RGB) all work, payload returns cleanly and BIOS reboots
- [ ] Serial payload bootloader (native USB) — not yet exercised on C3; see Known limitation below
- [ ] OTA update + rollback — not yet exercised on C3

### Confirmed working end-to-end (2026-07-13)

Full boot chain validated on real ESP32-C3 SuperMini hardware: cold boot →
BOOT-button interactive menu → WiFi STA connect (WPA2-PSK) → PXE HTTP
download → filesystem save → RAM payload launch → payload ABI calls → clean
return → BIOS reboot. No crashes, no brownouts, no hangs.

Two real bugs were found and fixed along the way (not port-specific, exist on
C6 too):
- **`components/web_ui/www/index.html`**: `text-transform: uppercase` CSS was
  applied to all `<input>` fields including SSID/password/PXE-URL, visually
  hiding the actual case of what was typed (the stored value was always
  correct — only the display was misleading). Fixed by exempting those three
  fields with `style="text-transform: none;"`.
