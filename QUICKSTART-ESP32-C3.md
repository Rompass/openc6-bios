# OpenC6 BIOS for ESP32-C3 SuperMini — Quick Start

Same codebase as the ESP32-C6 build, targeted at ESP32-C3 via
`CONFIG_IDF_TARGET`. See [ESP32-C3-SUPERMINI-PORT.md](ESP32-C3-SUPERMINI-PORT.md)
for the technical rationale behind each difference below.

## Prerequisites

- ESP32-C3 SuperMini board
- ESP-IDF v6.1 installed and sourced
- **A USB wall charger or powered hub.** Laptop USB ports frequently can't supply the current spike the board draws when WiFi/NVS-flash-erase kick in, causing brownout resets or the board dropping off USB entirely. This is a hardware limit, not a firmware bug — see the Power section below.

## Step 1: Switch Build Target to ESP32-C3

Edit `sdkconfig.defaults`:

```bash
sed -i 's/CONFIG_IDF_TARGET="esp32c6"/CONFIG_IDF_TARGET="esp32c3"/' sdkconfig.defaults
```

## Step 2: Set Target and Partition Table

```bash
. $IDF_PATH/export.sh
idf.py set-target esp32c3
idf.py set-partition-table partitions_c3_4mb.csv
```

`partitions_c3_4mb.csv` is a dedicated 4 MB layout — the default `partitions.csv` is sized for the C6's 8 MB flash and won't fit.

## Step 3: Build

```bash
idf.py build
```

In menuconfig, the following should already be set correctly for C3 via `sdkconfig.defaults`, but worth knowing if you troubleshoot:
- **Component config → ULP Co-processor** should be disabled (C3 has no LP-Core)
- **ESP System Settings → Memory Protection** must stay disabled (payloads execute from RAM)
- **Serial flasher config → Flash size** should read 4 MB

## Step 4: Flash

```bash
idf.py -p /dev/cu.usbmodem2101 flash
```

(Port name varies — check with `command ls /dev/cu.usbmodem*`, or `command ls /dev/tty.usbmodem*` on Linux-style naming. Note: if your shell aliases `ls` to something like `eza`/`exa` with color/icon output, use `command ls` to get a plain path.)

**If the board keeps dropping off USB mid-flash or resetting in a loop:** this is the power delivery issue mentioned above. Move it to a USB wall charger or powered hub. If you must flash while still on marginal power, retrying in a loop that watches for the port can occasionally catch a stable window, but it's not reliable — fix the power source instead.

## Step 5: Watch Boot / Enter Setup

```bash
idf.py -p /dev/cu.usbmodem2101 monitor
```
(`idf.py monitor` needs a real interactive terminal — it will error with "requires standard input to be attached to TTY" if run from a non-interactive context. Use `screen /dev/cu.usbmodem2101 115200` as a fallback.)

You should see BIOS init logs, then it goes to sleep waiting for the power
button (default `DC_LOSS_ACTION = POWER_OFF` behavior) unless you've already
saved different settings in NVRAM (e.g. after a prior Setup session).

### Entering BIOS Setup

- **Power button:** hold for 3 seconds from a powered-off (S5) state.
- **BOOT button (GPIO 9):** hold for 1 second at cold boot to open the Interactive Boot Menu, short-press to cycle options (LED blinks N+1 times for option N), long-press (>1s) to select. Option 4 (5 blinks) is BIOS Setup.

Either path brings up a WiFi AP named **`BIOS_SETUP_C3`**, password **`12345678`**. Connect and open **`http://192.168.4.1`**.

> A `BIOS_C3_ALWAYS_SETUP` bring-up flag existed briefly during initial C3
> bring-up (forced every cold boot straight into Setup, skipping the normal
> boot flow). It has been removed — it also intercepted saved settings like
> Network Boot, since a Setup "Save & Exit" reboot is a software reset and
> still counts as a cold boot in `bios_core_start()`'s logic. Normal boot flow
> (respecting NVRAM `DC_LOSS_ACTION` and boot-menu selections) is restored.

## Power delivery details

Symptoms if the power source is too weak: the board boots, prints BIOS logs,
then resets right as NVRAM/WiFi init starts (`esp_reset_reason()` reports
`ESP_RST_BROWNOUT` or `ESP_RST_PWR_GLITCH`), and its USB-CDC port drops and
reappears on a repeating cycle (commonly every few seconds). Brownout
detection is intentionally left **enabled** in this codebase — disabling it
only trades a clean, recoverable reset for a silent hang, it does not fix the
sag. The real fix is a steadier power source: a USB wall charger or powered
hub instead of a laptop port directly.

## Hardware Pinout Reference

| Function | GPIO | Note |
|----------|------|------|
| Power Button | 4 | Short press = boot, 3s hold = setup, 5s = hard reset |
| Power GND | 3 | Connect to GPIO 4 via pushbutton |
| Clear NVRAM | 2 | Short GPIO 2↔1 during boot to reset config |
| CMOS GND | 1 | Ground reference for GPIO 2 |
| POST LED | 8 | Addressable RGB (GBR layout, WS2812B compatible) — same pin as C6 |
| BOOT Button | 9 | Hold at cold boot to enter Interactive Boot Menu |

## Testing Checklist

- [x] BIOS boots without brownout resets (steady power)
- [x] POST LED (GPIO 8) responds
- [x] WiFi AP `BIOS_SETUP_C3` appears, password `12345678`
- [x] Web UI reachable at `http://192.168.4.1`
- [x] WiFi STA connects to a WPA2-PSK network
- [x] Network boot (PXE) downloads and launches a payload from RAM
- [ ] Serial payload bootloader (native USB)
- [ ] OTA update + rollback

**Tip:** if WiFi STA connection fails, double check the SSID/password you
typed in Setup — the fields previously displayed everything in uppercase due
to a CSS bug (now fixed), which could make you type/misread the wrong case
for a case-sensitive SSID or password.

## Common Issues

**"Failed to load ME binary" / Management Engine warnings:** expected on C3 —
there's no LP-Core, so the Management Engine always runs in Legacy Mode. Not
an error condition.

**Build fails with unknown target:** ensure ESP-IDF v6.1+ is sourced:
`. $IDF_PATH/export.sh && idf.py --version`.

**No console output / serial loader not responding:** the console and loader use
native USB register access that is hard-coded for the C6 and has not been
re-validated on C3 — see "Known limitation" in
[ESP32-C3-SUPERMINI-PORT.md](ESP32-C3-SUPERMINI-PORT.md).

**Flash size / partition mismatch:** confirm both `sdkconfig` shows
`CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y` and the partition table in use is
`partitions_c3_4mb.csv`, not the default 8 MB `partitions.csv`.

## Next Steps

1. **Develop payloads:** [docs/payload_development_serial_boot.md](docs/payload_development_serial_boot.md)
2. **Network boot:** [docs/pxe_network_boot.md](docs/pxe_network_boot.md)
3. **Technical rationale for the port:** [ESP32-C3-SUPERMINI-PORT.md](ESP32-C3-SUPERMINI-PORT.md)
