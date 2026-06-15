# OpenC6 PXE Network Boot & Wireless BIOS OTA Subsystem

The Network Deployment and Update module (`pxe_boot.c` and `pxe_boot.h`) manages the wireless acquisition of binaries over HTTP. It operates on two distinct execution paths: downloading volatile/non-volatile user payloads (PXE Boot) to execute via Execute-In-Place (XIP), and performing host-system firmware self-updates (BIOS OTA) utilizing the ESP32-C6 hardware partition table controllers.

---

## 1. Subsystem Architecture & Execution Targets

The network subsystem interacts with different physical flash targets depending on the operational context:

| Operation Target        | Destination Partition | Partition Type | Mapping Method | Next Boot Step                    |
| :---------------------- | :-------------------- | :------------: | :------------- | :-------------------------------- |
| Payload Network Boot    | `network_buf`         |      Data      | Virtual MMU    | Direct XIP branch jump            |
| System BIOS Self-Update | `ota_0` / `ota_1`     |      App       | OTA Controller | Boot partition vector redirection |

---

## 2. Wireless PXE Payload Deployment

When the Boot Dispatcher triggers a PXE Boot, the system establishes a temporary HTTP connection to fetch a flat machine-code payload binary.
```text
   [ HTTP Download Request ]
              │
              ▼
   [ Read HTTP Content-Length ]
              │
              ▼
 [ Align size to 4KB sectors ]
              │
              ▼
  [ Erase 'network_buf' range ]
              │
              ▼
[ Download & Write Chunks (1KB) ]
              │
              ▼
   [ Validate Total Bytes ]
              │
              ▼
   [ Handover to XIP Boot ]

```
### 2.1 Core Implementation Details:
* **Connection Handshake:** Configures the client with a 5-second connection timeout (`timeout_ms = 5000`) and disables HTTP keep-alive to preserve memory buffers.
* **Flash Alignment Calculation:** Flash memory must be erased in 4 KB sectors. To prevent erasing adjacent partitions or under-allocating, the BIOS calculates the required erase range using bitwise alignment:

  Erase_Size = (Content_Length + 4095) AND (NOT 4095)

This guarantees that only the exact number of sectors containing the binary are
formatted.

  - Chunked Streaming: The payload is streamed in 1024-byte blocks. Telemetry
    logs are printed every 10 KB to provide dynamic progress feedback.

3. Wireless System Firmware Self-Update (BIOS OTA)

The host BIOS can update its own compiled firmware image safely over the
network. It protects the system from corruption using an A/B rollback layout.

3.1 Passive Partition Routing

The BIOS never overwrites the active running partition. It queries the partition
table to locate the inactive storage block:

const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);

The update is written to this passive slot, allowing the system to fall back to
the safe, previously running version if the new image fails to boot.

3.2 Secure OTA Flashing Flow:

1.  Target Allocation: Initializes the OTA handler via esp_ota_begin() which
    formats the target inactive app partition.
2.  Sequential Streaming: Streams the binary over HTTP in 1024-byte blocks and
    writes them directly to flash via esp_ota_write(). Telemetry logs are
    updated every 50 KB.
3.  Validation and Commit: Once the download matches the HTTP headers, the
    transaction is finalized via esp_ota_end().
4.  Boot Vector Swap: Calls esp_ota_set_boot_partition() to redirect the
    second-stage bootloader's boot vectors to the new partition.
5.  Rollback Safety Net: Upon the next boot, the new firmware runs in
    PENDING_VERIFY state. If the LP-Core Management Engine detects a crash or
    watchdog freeze, the hardware bootloader automatically reverts to the stable
    factory partition.

4. API Reference

4.1 PXE Boot Executor

bool pxe_boot_execute(const char* url);

Connects to a remote server, prepares the local network_buf flash storage,
downloads a bare-metal payload, and validates execution bounds.

  - Parameter: url — Target HTTP address of the payload binary (e.g.,
    http://192.168.1.1:8080/payload.bin).
  - Return Value: true if the download, write, and verification steps succeed.

4.2 System BIOS OTA Flasher

bool pxe_bios_ota_execute(const char* url);

Connects to a remote update server, resolves the current inactive A/B app slot,
streams the new system BIOS firmware, and updates the bootloader state flags.

  - Parameter: url — Target HTTP address of the compiled BIOS update image.
  - Return Value: true if the system image is successfully written, validated,
    and set as active.

---
