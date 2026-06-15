# OpenC6 BIOS Setup Utility: Retro Web UI & API Router

The BIOS Setup Utility (`web_server.c`, `web_ui.h`, and `index.html`) hosts an integrated configuration server on port 80. Designed as a localized Access Point service, this module fulfills two functions: it serves a retro blue-background Setup console mimicking classic x86 PC BIOS layouts, and it exposes a lightweight JSON REST API for real-time telemetry extraction, system-state controls, and NVRAM synchronization.

---

## 1. REST API Routing Specifications

The HTTP daemon operates an asynchronous router to process control actions, fetch system states, or update variables in NVRAM.

| API Endpoint       | Method | Request Payload | Response Format    | System Action                                       |
| :----------------- | :----: | :-------------- | :----------------- | :-------------------------------------------------- |
| `/`                | `GET`  | None            | `text/html`        | Serves the embedded retro BIOS HTML page            |
| `/api/sysinfo`     | `GET`  | None            | `application/json` | Gathers real-time telemetry (CPU, RAM, Temp, NVS)   |
| `/api/settings`    | `GET`  | None            | `application/json` | Gathers active configuration parameters from NVS    |
| `/api/settings`    | `POST` | JSON Object     | `text/plain`       | Commits client configurations to NVRAM variables    |
| `/api/reboot`      | `POST` | None            | `text/plain`       | Restarts the ESP32-C6 SoC                            |
| `/api/defaults`    | `POST` | None            | `text/plain`       | Resets NVS parameters to baseline factory defaults  |
| `/api/trigger_ota` | `POST` | None            | `text/plain`       | Triggers network BIOS firmware update, then reboots |

---

## 2. Telemetry Generation & JSON Mapping

The `/api/sysinfo` route aggregates hardware metrics directly from low-level controllers and compiles them into a structured JSON string using the `cJSON` library:

1. **Junction Temperature:** Fetches the real-time core temperature parameter directly from the shared LP-SRAM location `ulp_me_temperature`.
2. **CPU Clock Rate:** Retrieves the calibrated tick frequency of the active core by calling the ROM-level API `esp_rom_get_cpu_ticks_per_us()`.
3. **Internal SRAM Statistics:** Measures free heap cap blocks using the standard allocator flags `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`.
4. **Dynamic Partition Table Scan:** Iterates through the partition tables at runtime using the `esp_partition_iterator` driver to construct an array containing partition labels, flash offset addresses, and sizes.
5. **NVS Sector Wear-Leveling Metrics:** Accesses structural statistics of the active config partition using `nvs_get_stats()` to track total entries used vs. empty blocks.

---

## 3. The Retro Web Interface Design (`index.html`)

The user interface is entirely self-contained inside an embedded, single-page document (`index.html`) mapped directly into the binary partition of the BIOS firmware via CMake.
```text
   ┌────────────────────────────────────────────────────────┐
   │                 OpenC6 Web UI Setup                    │
   │                                                        │
   │  System Health Monitor  <─── [1s Auto Refresh Loop]    │
   │  Storage & Partitions   <─── [Dynamic Table Render]    │
   │  AI Tweaker Configs     <─── [Form Selectors]          │
   │  Lighting & Network     <─── [Form Inputs]             │
   │                                                        │
   │   [ F10: Save ]    [ F9: Defaults ]   [ F12: Update ]  │
   └────────────────────────────────────────────────────────┘
```

### 3.1 Styling and Usability Highlights:
* **Classic Retro Look:** Powered by CSS rules enforcing monospaced Courier New fonts, standard deep-blue backgrounds (`#0000AA`), yellow highlighted captions (`#FFFF55`), and blocky shadowed panels to match old-school setups.
* **Auto-Refresh Loop:** Features a non-blocking JavaScript routine that polls the `/api/sysinfo` endpoint every **1000 ms** to update temperature readouts, RAM allocation tables, and the visual NVS storage progress bar in real-time.
* **System Event Mapping:** Includes keyboard-mapped control triggers:
  * **F10 (Save & Exit):** Serializes all configuration inputs into a single JSON object and POSTs it to the `/api/settings` API route.
  * **F9 (Load Defaults):** Sends a clean-up trigger to reset variables to factory-safe values, then forces a client-side page reload.
  * **F12 (Network Update):** Validates the server target URL and initiates the wireless BIOS OTA flash sequence.

---

## 4. Stack Allocation & JSON Safety

Parsing complex, nested JSON payloads can be exceptionally memory-intensive. Using `cJSON_Parse()` on an active thread with default system parameters can easily trigger a stack overflow.

To prevent RTOS scheduler crashes under heavy HTTP client loads, the BIOS configures a dedicated stack size during web server initialization:

httpd_config_t config = HTTPD_DEFAULT_CONFIG();
config.stack_size = 10240; // Spawns server threads with 10 KB stack pools

This 10 KB stack allocation ensures ample headroom for dynamic JSON printing,
string concatenation, and memory allocation.

5. API Reference

5.1 BIOS Web Configurator Initiator

void web_ui_start(void);

Starts the active HTTP web server on port 80.

  - Actions: Allocates heap pools, loads dynamic route handlers, sets the stack
    size boundary, and begins listening on standard port 80.
  - Requirement: The Wi-Fi controller must be successfully initialized and
    configured as an Access Point (SoftAP) or a Station (STA) before this
    function is invoked.

---
