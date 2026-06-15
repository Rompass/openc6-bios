# OpenC6 Wi-Fi Management: Network Stack & Connectivity Engine

The Wi-Fi Management module (`wifi_mgmt.c` and `wifi_mgmt.h`) handles wireless configuration and stack initialization. Operating on top of the ESP-IDF TCP/IP adapter (esp_netif) and the L2 Wi-Fi driver, this module exposes simple interfaces to host the local BIOS Setup Access Point (SoftAP mode) and connect to home routers for wireless PXE Boot and host BIOS self-updates (Station mode).

---

## 1. Safe Network Interface (netif) Re-use

In standard ESP-IDF applications, repeatedly toggling between Wi-Fi modes (such as switching from SoftAP back to Station mode) can result in duplicate initialization assertions or heap leaks if the default virtual network interfaces (netifs) are allocated multiple times. 

To prevent this, the BIOS checks for existing interface descriptors using key-value bindings before invoking default creation routines:
```text
           [ Mode Transition Initiated ]
                         │
                         ▼
           ┌───────────────────────────┐
           │    Does netif handle      │
           │    exist in database?     │
           └─────────────┬─────────────┘
                         │
           ┌─────────────┴─────────────┐
           ▼ Yes                       ▼ No
   ┌────────────────┐          ┌───────────────────────────┐
   │ Re-use handle  │          │ Allocate and register     │
   │ from key store │          │ new default netif handle  │
   └────────────────┘          └───────────────────────────┘
```

The keys used for this check are:
* **`WIFI_AP_DEF`** (Default SoftAP handle)
* **`WIFI_STA_DEF`** (Default Station handle)

If `esp_netif_get_handle_from_ifkey()` returns a valid descriptor, the BIOS retains the existing handle and bypasses allocation. If it returns `NULL`, the interface is safely initialized via `esp_netif_create_default_wifi_ap()` or `esp_netif_create_default_wifi_sta()`.

---

## 2. Event-Driven Thread Synchronization

The network stack operates asynchronously via the default system event loop. To allow the synchronous BIOS Boot Dispatcher to block until a network handshake succeeds, the module implements a FreeRTOS Event Group (`s_wifi_event_group`) utilizing two synchronization bits:

* **`WIFI_CONNECTED_BIT` (BIT0):** Flagged when the TCP/IP stack successfully acquires an IP address (`IP_EVENT_STA_GOT_IP`).
* **`WIFI_FAIL_BIT` (BIT1):** Flagged if the physical layer fails to authenticate or connect with the target router (`WIFI_EVENT_STA_DISCONNECTED`).

### Synchronous Handshake Sequence
Inside `wifi_mgmt_start_sta()`, the executing task suspends and waits for up to **10 seconds** for either of these bits to be set:

EventBits_t bits = xEventGroupWaitBits(
    s_wifi_event_group, 
    WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, 
    pdFALSE, 
    pdFALSE, 
    pdMS_TO_TICKS(10000)
);

If WIFI_CONNECTED_BIT is received, the connection is validated and the function
returns ESP_OK. If the timeout is reached or WIFI_FAIL_BIT is asserted, it logs
a failure and returns ESP_FAIL.

3. Supported Security Modes

3.1 SoftAP Mode (Access Point)

Spawns a local configuration AP with the following operational constraints:

  - Dynamic Authentication: If no password is provided in the configuration
    call, the AP defaults to WIFI_AUTH_OPEN. Otherwise, it enforces
    WIFI_AUTH_WPA2_PSK security.
  - Protected Management Frames (PMF): PMF is configured as required
    (pmf_cfg.required = true) to prevent packet-deauthentication exploits.
  - Channel and Connections: Locked to Channel 1 with a maximum client limit of
    4 concurrent connections to minimize SRAM overhead.

3.2 Station Mode (STA Client)

Enables backward-compatible security Handshakes on the ESP32-C6:

  - SAE (WPA3-Personal) Support: Configured with WPA3_SAE_PWE_BOTH
    (Hash-to-Element and Hunting-and-Pecking), allowing robust handshakes on
    modern WPA3-only or mixed WPA2/WPA3 access points.
  - Authentication Threshold: Set to a minimum of WIFI_AUTH_WPA2_PSK to prevent
    connection to insecure open networks.

4. API Reference

4.1 Subsystem Initializer

void wifi_mgmt_init(void);

Initializes the global TCP/IP adapter layer, spawns the default system event
loop, configures the internal Wi-Fi stack memory pools, and registers callback
instances for WIFI_EVENT and IP_EVENT bases. Protected by a static
initialization guard to prevent multiple allocations.

4.2 SoftAP Mode Controller

esp_err_t wifi_mgmt_start_ap(const char* ssid, const char* pass);

Stops active Wi-Fi transitions, retrieves or registers the "WIFI_AP_DEF"
interface handle, configures the SoftAP security credentials, and activates the
wireless access point.

  - Parameters:
      - ssid — Target network identifier (SSID).
      - pass — Target password (SSID authentication). Pass "" for an open
        network.
  - Return Value: ESP_OK upon successful start.

4.3 Station Mode Controller

esp_err_t wifi_mgmt_start_sta(void);

Retrieves or registers the "WIFI_STA_DEF" interface, reads Wi-Fi credentials
dynamically from NVRAM, configures the station client with WPA2/WPA3 SAE
compatibility, initiates the physical connection, and blocks until an IP is
acquired or the 10-second timeout occurs.

  - Return Value: ESP_OK if connection is established and an IP is acquired;
    ESP_FAIL if credentials are empty, the handshake fails, or the timeout is
    reached.

4.4 Connection Status Checker

bool wifi_mgmt_is_connected(void);

Checks the current state of the Station interface.

  - Return Value: true if WIFI_CONNECTED_BIT is active; false otherwise.

---
