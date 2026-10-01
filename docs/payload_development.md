# OpenC6 Payload Development and ABI Specification

This document details the execution model, system call interface, Application Binary Interface (ABI), memory layout, build system, and deployment process for building bare-metal RISC-V payloads targeting the OpenC6 microkernel sandbox.

---

## 1. Execution Model and Security Architecture

OpenC6 isolates user applications from the host kernel using the hardware **RISC-V Physical Memory Protection (PMP)** unit in conjunction with unprivileged **User Mode (`U-Mode`)**.

### Privilege Level
Payloads execute strictly in RISC-V User Mode (`U-Mode`). Any attempt to execute privileged Machine Mode instructions (`csrw`, `csrr`, `mret`, `wfi`) immediately triggers an **Illegal Instruction Fault (mcause: 2)**. The kernel intercepts the trap, outputs a diagnostic register dump, and safely terminates the faulting process.

### Memory Boundaries (PMP Top-of-Range Configuration)
The kernel dynamically sets PMP entries (Entries 4..7) using Top-of-Range (`TOR`) addressing:
* **Payload Arena:** Allocated dynamically from internal SRAM. The payload has full Read, Write, and Execute (`RWX`) permissions within its assigned arena slice.
* **Kernel SRAM:** Strictly inaccessible (Permissions: `NONE`). Writing or reading outside the arena generates a **Store Access Fault (mcause: 7)** or **Load Access Fault (mcause: 5)**.
* **Peripheral MMIO Space (`0x60000000 - 0x600FFFFF`):** Blocked by Default Deny. Payloads cannot access SoC hardware registers directly and must use kernel ABI syscalls.
* **Flash Memory (IROM/DROM):** Marked Read and Execute (`R-X`) for ABI table dereferences and kernel function trampolines.

### Concurrency and Process Slots
* OpenC6 supports up to **8 concurrent process slots** (`OPENC6_MAX_PROCS`).
* Memory is managed through a contiguous 4 KB page pool using Next-Fit allocation.
* Preemptive context switching is driven by the hardware SysTick timer.
* FreeRTOS interrupt privilege leakage on RISC-V is mitigated via the kernel's `sandbox_intr_trampoline`.

---

## 2. Binary Structure and Linker Conventions

Payloads must be compiled as position-independent, freestanding flat binary images (`.bin`).

### Memory Layout
Because payloads can be placed at dynamic physical addresses in SRAM depending on available page slots, all code must use position-independent addressing (`-fPIC`). Absolute jump tables and tree-switch conversions must be disabled to prevent unresolved relocations.

The entry point must be located at offset `0x0` of the flat image. The linker configuration is defined in [`tools/payload.ld`](../tools/payload.ld):
* `.text.entry` is mapped strictly to the base address (`. = 0x0`).
* Standard `.text`, `.rodata`, `.data`, and `.bss` follow sequentially.
* Debug frames and comments are stripped via `/DISCARD/`.

---

## 3. Entry Point and ABI Handshake

When a payload starts, the OpenC6 microkernel passes a pointer to the system dispatch structure in register `a0`. The payload entry function must be declared in section `.text.entry`.

### Entry Signature
```c
#include "openc6_abi.h"

void __attribute__((section(".text.entry"), noreturn)) payload_main(const openc6_abi_t *abi)
{
    /* Validate structural boundary integrity */
    if (!abi || abi->magic != OPENC6_ABI_MAGIC || abi->version != OPENC6_ABI_VERSION) {
        while (1) {
            __asm__ volatile("nop");
        }
    }

    /* Payload execution logic */
    abi->print("Payload active.\r\n");
    abi->sys_reset();
}
```

* `OPENC6_ABI_MAGIC`: `0x43364249` (ASCII: `C6BI`)
* `OPENC6_ABI_VERSION`: `2`

---

## 4. ABI Function Reference (`openc6_abi_t`)

The system dispatch table provides direct C function pointers to kernel services. Under the hood, these wrappers invoke RISC-V `ecall` instructions to safely transition into Machine Mode.

### 4.1 System and Memory Services

* `void sys_reset(void)` — Terminates the payload and yields execution back to the host shell without resetting the SoC.
* `void print(const char *str)` — Transmits a null-terminated string to the bare-metal USB-Serial-JTAG CDC FIFO and mirrors it to the remote Web Shell (`c6wsh`).
* `void delay_ms(uint32_t ms)` — Suspends the calling task for a given number of milliseconds.
* `void* malloc(uint32_t size)` — Allocates dynamic memory from the process-local heap area inside the assigned sandbox arena. Returns `NULL` on failure.
* `void free(void *ptr)` — Returns dynamic memory back to the process-local free list.
* `uint32_t get_free_ram(void)` — Returns total free internal DRAM of the SoC in bytes.
* `uint32_t get_total_ram(void)` — Returns total capacity of internal DRAM in bytes.
* `uint32_t get_total_flash(void)` — Returns physical capacity of the onboard SPI Flash in bytes.

---

### 4.2 Hardware Peripherals and GPIO

#### Safe GPIO Model (`hal_gpio.h`)
OpenC6 enforces a hardware protection bitmask (`OPENC6_GPIO_PROTECTED_MASK`). The following pins are locked by the kernel and return `-1` on any access attempt:
* **GPIO 1, 2:** Hardware Clear CMOS jumpers.
* **GPIO 3, 4:** LP-Core Management Engine sense and virtual ground lines.
* **GPIO 8:** WS2812 status LED (reserved for kernel Aura Sync).
* **GPIO 9:** Physical BOOT button.
* **GPIO 12, 13:** Native USB-Serial-JTAG D- and D+ lines.
* **GPIO 24..30:** High-speed SPI Flash bus.

All other pins (GPIO 0, 5, 6, 7, 10, 11, 14..23) are available for user applications.

* `int32_t gpio_set_dir(uint32_t pin, uint32_t is_output)` — Configures direction (`1` = Output, `0` = Input). Returns `0` on success, `-1` if protected.
* `int32_t gpio_write(uint32_t pin, uint32_t level)` — Sets output level (`1` = 3.3V, `0` = 0V) using atomic registers. Returns `0` on success, `-1` if protected.
* `int32_t gpio_read(uint32_t pin)` — Reads digital logic level directly from hardware registers. Returns `0` or `1`, or `-1` if protected.
* `void set_led_color(uint8_t r, uint8_t g, uint8_t b)` — Updates the onboard WS2812 addressable RGB LED color (`0..255`).
* `uint32_t get_random(void)` — Fetches a 32-bit hardware true random number from the ESP32-C6 analog RF thermal noise generator (`WDEV_RND_REG`).

---

### 4.3 Cryptography and Mathematics

* `void sha256(const uint8_t *input, uint32_t len, uint8_t *output)` — Computes a standalone SHA-256 digest (32 bytes) executed in Machine Mode.
* `uint32_t math_isqrt(uint32_t x)` — Fast integer square root calculation.
* `int32_t math_sin_deg(int32_t angle_deg)` — Fixed-point sine calculation. Returns `sin(angle) * 10000`.
* `int32_t math_cos_deg(int32_t angle_deg)` — Fixed-point cosine calculation. Returns `cos(angle) * 10000`.

---

### 4.4 Networking and BSD Sockets

Payloads can create TCP servers and handle client connections concurrently while running under PMP protection.

* `int32_t wifi_is_connected(void)` — Returns `1` if the Wi-Fi station holds a valid IPv4 address, `0` otherwise.
* `int32_t net_get_ip(char *buf, uint32_t max_len)` — Copies the active IPv4 address string into `buf`. Returns `0` on success, `-1` on failure.
* `int32_t tcp_listen(uint16_t port)` — Creates, binds, and configures a listening TCP socket on the specified port. Returns server fd (`>= 0`) or `-1`.
* `int32_t tcp_accept(int32_t server_fd, uint32_t timeout_ms)` — Waits for an incoming client connection. Returns client fd (`>= 0`) or `-1`.
* `int32_t tcp_read(int32_t fd, void *buf, uint32_t max_len)` — Reads incoming data from a connected client. Returns byte count, `0` on disconnect, or `-1`.
* `int32_t tcp_write(int32_t fd, const void *buf, uint32_t len)` — Sends data buffer to a connected client. Returns bytes transmitted or `-1`.
* `void tcp_close(int32_t fd)` — Shuts down and closes the socket descriptor.

---

### 4.5 Log-Structured Virtual Filesystem (`openc6_fs`)

* `void fs_write_file(const char *name, const uint8_t *data, uint32_t len, uint32_t dir_sector, uint8_t force)` — Writes or overwrites a file into a directory sector.
* `int32_t fs_read_file(const char *name, uint8_t *dest, uint32_t offset, uint32_t len, uint32_t dir_sector)` — Reads file data into `dest`. Returns bytes read or `-1`.
* `void fs_delete(const char *name, uint32_t dir_sector)` — Deletes the file entry from Flash storage.

---

## 5. Direct Syscall Table (`openc6_syscall.h`)

Payloads writing assembly or implementing custom runtime environments can issue direct kernel traps via `ecall` with register conventions: `a7` = Syscall ID, `a0..a3` = Arguments. Return values are passed back in `a0`.

| Syscall ID | Constant | Input Parameters | Output (`a0`) |
|---|---|---|---|
| `0` | `SYS_EXIT` | `a0: int exit_code` | — |
| `1` | `SYS_PRINT` | `a0: const char *str` | `0` |
| `2` | `SYS_DELAY_MS` | `a0: uint32_t ms` | `0` |
| `3` | `SYS_SYS_RESET` | — | — |
| `4` | `SYS_SET_LED_COLOR` | `a0: r, a1: g, a2: b` | `0` |
| `5` | `SYS_GET_RANDOM` | — | `uint32_t val` |
| `6` | `SYS_SHA256` | `a0: in, a1: len, a2: out` | `0` or `-1` |
| `7` | `SYS_MATH_ISQRT` | `a0: uint32_t val` | `uint32_t result` |
| `8` | `SYS_MATH_SIN_DEG`| `a0: int32_t deg` | `int32_t result` |
| `9` | `SYS_MATH_COS_DEG`| `a0: int32_t deg` | `int32_t result` |
| `10`| `SYS_NET_GET_IP` | `a0: char *buf, a1: max_len` | `0` or `-1` |
| `12`| `SYS_WIFI_IS_CONNECTED` | — | `1` or `0` |
| `13`| `SYS_GET_FREE_RAM` | — | `uint32_t bytes` |
| `14`| `SYS_GET_TOTAL_RAM`| — | `uint32_t bytes` |
| `15`| `SYS_GET_TOTAL_FLASH` | — | `uint32_t bytes` |
| `16`| `SYS_FS_WRITE_FILE`| `a0: name, a1: data, a2: len, a3: dir` | — |
| `17`| `SYS_FS_READ_FILE` | `a0: name, a1: dest, a2: off, a3: len` | `int32_t bytes` |
| `18`| `SYS_FS_DELETE` | `a0: name, a1: dir` | — |
| `19`| `SYS_SBRK` | `a0: intptr_t increment` | `uintptr_t prev_break` |
| `20`| `SYS_TCP_LISTEN` | `a0: uint16_t port` | `int32_t server_fd` |
| `21`| `SYS_TCP_ACCEPT` | `a0: fd, a1: timeout_ms` | `int32_t client_fd` |
| `22`| `SYS_TCP_READ` | `a0: fd, a1: buf, a2: len` | `int32_t bytes_read` |
| `23`| `SYS_TCP_WRITE` | `a0: fd, a1: buf, a2: len` | `int32_t bytes_sent` |
| `24`| `SYS_TCP_CLOSE` | `a0: int fd` | `0` |
| `30`| `SYS_MALLOC` | `a0: uint32_t size` | `void *ptr` |
| `31`| `SYS_FREE` | `a0: void *ptr` | `0` |
| `32`| `SYS_GPIO_SET_DIR`| `a0: pin, a1: is_out` | `0` or `-1` |
| `33`| `SYS_GPIO_WRITE` | `a0: pin, a1: level` | `0` or `-1` |
| `34`| `SYS_GPIO_READ` | `a0: pin` | `1`, `0`, or `-1` |

---

## 6. Build System and Compilation

### Toolchain Requirements
* **ESP-IDF v6.1+** environment sourced (`. $IDF_PATH/export.sh`).
* Required binary tools: `riscv32-esp-elf-gcc`, `riscv32-esp-elf-objcopy`.

### Automated Build (`tools/`)
The `tools/` directory includes an integrated build system. Any `.c` source placed in `tools/example/` is automatically discovered and compiled into a flat `.bin` binary:

```bash
cd tools

# Build host tools and cross-compile all example/*.c payloads
make
```

Output binaries are placed in:
`tools/build/bin/payloads/<payload_name>.bin`

### Manual Compilation
To compile a payload outside the automated build tree, use the exact compiler flags defined in [`tools/CMakeLists.txt`](../tools/CMakeLists.txt):

```bash
riscv32-esp-elf-gcc \
    -march=rv32imac \
    -mabi=ilp32 \
    -Os \
    -fPIC \
    -fno-jump-tables \
    -fno-tree-switch-conversion \
    -nostdlib \
    -I../components/openc6_abi/include \
    -Wl,-T,payload.ld \
    -Wl,--no-warn-rwx-segments \
    -o payload.elf payload.c

riscv32-esp-elf-objcopy -O binary payload.elf payload.bin
```

---

## 7. Reference Implementations

Full, production-ready reference payloads are provided in the repository under [`tools/example/`](../tools/example/):

* **[`tools/example/payload.c`](../tools/example/payload.c) — Diagnostic & PMP Security Suite:**  
  Demonstrates ABI handshake validation, dynamic DRAM queries, heap allocations (`malloc`/`free`), hardware TRNG, SHA-256 accelerator hashing, fixed-point math, an embedded HTTP web server on port 443, RGB sequences, and deliberate PMP security testing (triggering a controlled write to protected kernel SRAM at `0x40800000`).

* **[`tools/example/payload2.c`](../tools/example/payload2.c) — Persistent Background HTTP Daemon:**  
  Demonstrates deploying a non-terminating network daemon listening on port 8080. Designed for background execution (`boot payload2.bin bg`), handling incoming HTTP requests while remaining fully isolated under PMP boundaries.

* **[`tools/example/payload3.c`](../tools/example/payload3.c) — Hardware GPIO & Pin Security Suite:**  
  Demonstrates the kernel hardware pin protection audit (testing access rejection across protected lines: CMOS, LP-Core, BOOT, Flash, USB, WS2812) and output square-wave generation on accessible user GPIOs.

---

## 8. Payload Compression (ZC6 Format)

OpenC6 includes the **ZC6 v4** custom compression utility (`tools/zc6_pack`) optimized for 32-bit RISC-V binary patterns (RLE, sparse 32-bit words, TinyLZ).

When a `.zc6` file is launched via `boot <file.zc6>`, the OpenC6 process manager automatically decompresses it directly into assigned 4 KB RAM pages prior to launch:

```bash
# Compress flat binary into .zc6 archive
./tools/build/bin/zc6_pack build/bin/payloads/payload.bin payload.zc6

# Transfer compressed file to OpenC6 filesystem
openc6_fs [Dir: 0] /> serial payload.zc6
./build/bin/openc6_loader /dev/ttyACM0 payload.zc6

# Launch directly (decompressed transparently into memory pages)
openc6_fs [Dir: 0] /> boot payload.zc6
```

---

## 9. Deployment and Process Control

### Streaming over Type-C (Serial)
1. In the OpenC6 shell, initialize the serial receiver:
   ```text
   openc6_fs [Dir: 0] /> serial my_app.bin
   ```
2. On your host workstation, run the loader:
   ```bash
   ./tools/build/bin/openc6_loader /dev/ttyACM0 tools/build/bin/payloads/payload.bin
   ```
3. Run the binary in the foreground:
   ```text
   openc6_fs [Dir: 0] /> boot my_app.bin
   ```

### Preemptive Background Execution and Job Control
Launch persistent daemons directly in the background using `bg` or `&`:
```text
openc6_fs [Dir: 0] /> boot my_app.bin bg
[JOB 1] Running in background: 'my_app.bin' (Alloc: 16 KB [4 pages])
```

Interactive shell commands:
* `top` — Display active PIDs, CPU governor load, runtime, and memory page allocation.
* `suspend <pid>` (or `Ctrl+X` for foreground tasks) — Freeze execution and compress pages into RAM via **ZSWAP**.
* `fg <pid>` — Bring a suspended or background process back to the active console.
* `bg <pid>` — Resume a suspended process in the background.
* `kill <pid>` — Terminate execution and release socket descriptors.

