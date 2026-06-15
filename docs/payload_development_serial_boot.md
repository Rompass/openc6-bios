# OpenC6 Payload Development & Serial Boot Guide

The `tools/` directory contains everything needed to build bare-metal RISC-V applications (Payloads) and deploy them to the ESP32-C6 via the custom Serial Boot protocol. It completely bypasses standard ESP-IDF flashing tools (`esptool.py`), allowing rapid development and hot-swapping of applications in volatile RAM or persistent Flash memory.

---

## 1. Toolchain & Compilation

Building the OpenC6 toolchain produces two distinct artifacts:
1. **`openc6_loader`**: A C++ host utility (runs on your PC) that implements the robust UART synchronization protocol.
2. **`payload.bin`**: The actual bare-metal RISC-V executable derived from `payload.c`.

### Build Instructions

Because the payload requires the ESP-IDF RISC-V cross-compiler (`riscv32-esp-elf-gcc`), you must activate the ESP-IDF environment before running CMake.

**Step 1: Activate ESP-IDF Environment**
```bash
# Navigate to your ESP-IDF installation and source the export script
. $IDF_PATH/export.sh
```
Step 2: Build the Tools and Payload

# Navigate to the tools directory in the OpenC6 repository
cd tools/

# Create a build directory and compile
mkdir build
cd build
cmake ..
make

Upon successful compilation, CMake automatically copies the openc6_loader
executable and payload.bin directly into the root of the tools/ directory for
easy access.

2. Serial Boot Deployment (UART)

To ensure the payload binary stream is not corrupted by standard ESP-IDF system
logs or USB-JTAG console output, the OpenC6 BIOS routes the Serial Bootloader
through an isolated hardware UART interface.

2.1 Hardware Setup (USB-to-TTL Adapter)

You will need an external USB-to-TTL Serial Adapter (e.g., based on the CP2102
or CH340 chip) to communicate with the Serial Bootloader.

Connect the adapter to the ESP32-C6 using the following pinout:

| CP2102 Adapter | ESP32-C6 Pin | Description              |
| :------------- | :----------- | :----------------------- |
| **GND**        | **GND**      | Common ground reference  |
| **RX (Read)**  | **GPIO 18**  | ESP32-C6 Hardware TX Pin |
| **TX (Write)** | **GPIO 19**  | ESP32-C6 Hardware RX Pin |

2.2 Step 1: Put ESP32-C6 into Serial Boot Mode

1.  Reset the board while holding the BOOT button (GPIO 9).
2.  The POST LED will begin blinking cyan.
3.  Use short clicks to navigate to one of the Serial Boot targets:
      - [1 Blink]: Serial Boot (RAM) - Deploys to fast, volatile execution SRAM.
        Resets on power loss.
      - [2 Blinks]: Serial Boot (FLASH) - Deploys to persistent network_buf
        flash storage for XIP (Execute-In-Place) booting.
4.  Long-press the BOOT button (>= 1 sec) to confirm. The LED will turn Magenta
    (RAM) or Pink (Flash), indicating the BIOS is waiting for the host sync.

2.3 Step 2: Run the Host Loader

Execute the openc6_loader on your PC, passing the active serial port of your
CP2102 adapter and the binary file.
```text
# Return to the tools directory
cd ..

# Run the loader (replace /dev/ttyUSB0 with your actual CP2102 port, e.g., COM3 on Windows)
./openc6_loader /dev/ttyUSB0 payload.bin

Expected Output:

==========================================
 OpenC6 BIOS - Payload Loader (Robust Sync)
==========================================
Waiting for Text Marker '##OPENC6_SYNC##' from BIOS...
BIOS is ready! Sending file size with Magic Preambule...
Waiting for BIOS to allocate RAM or Erase Flash...
Flashing Payload...
[==================================================] 100 %
Transmission 100% Complete.
EOT sent. BIOS is jumping to Payload!
SUCCESS.
```
3. Payload Architecture Under the Hood

Unlike standard ESP32 applications, OpenC6 payloads are standalone,
position-independent machine code blocks.

3.1 The Linker Script (payload.ld)

The linker script is the heart of the bare-metal application. It configures the
ELF output to start at virtual address 0x0.

  - .text (Code & Constants): Placed at the very beginning of the binary. The
    custom entry point .text.entry forces the payload_main() function to be the
    very first instruction executed when the BIOS jumps to the mapped memory.
  - .data and .bss (Variables): Initialized and zero-initialized variables.

CRITICAL ARCHITECTURE WARNING (RAM vs. Flash Targets): Because payloads are
compiled as Flat Binaries without an OS loader, there is no mechanism to copy
.data variables from ROM to RAM at startup.

  - If deployed to RAM (PAYLOAD_TARGET_RAM), you can safely read and write to
    global variables.
  - If deployed to Flash (PAYLOAD_TARGET_FLASH via XIP), the entire binary is
    mapped to the CPU Instruction Cache as Read-Only. Attempting to write to a
    global variable (e.g., int my_var = 5;) will trigger a hardware Store/AMO
    Access Fault and crash the system! Use ABI malloc() for dynamic read/write
    data when targeting Flash.

3.2 Compilation Flags (CMakeLists.txt)

The payload is compiled with highly specific GCC flags:

  - -nostdlib: Prevents linking the standard C library (libc). We cannot use
    printf() or malloc() from the standard library because they rely on an OS.
    We must use the BIOS ABI wrappers (abi->print(), abi->malloc()).
  - -fPIC -fno-jump-tables: Enforces absolute Position Independent Code. This
    ensures all function calls and branches use relative PC (Program Counter)
    offsets, allowing the binary to run flawlessly regardless of where the BIOS
    allocates it in physical memory.
  - -Wl,--no-warn-rwx-segments: Suppresses modern linker warnings regarding
    executable data segments, which are expected in our bare-metal flat binary
    deployment.

---
