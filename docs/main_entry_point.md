# OpenC6 BIOS Main Entry Point (main.c)

The `main.c` file serves as the definitive execution entry point for the ESP32-C6 High-Performance (HP) core upon the conclusion of the 2nd-stage bootloader phase. It functions as a lightweight bootstrapper, quickly shifting system execution from the default ESP-IDF startup routines over to the customized OpenC6 BIOS Core state machine.

---

## 1. Execution Flow & Handover

The ESP-IDF framework initializes system hardware, sets up basic RTOS tasks, and subsequently spawns the `main` task on the primary RISC-V HP-core, which invokes `app_main()`.
```text
 [ ESP-IDF 2nd Stage Bootloader ]
                │
                ▼
   [ Hardware & RTOS Init ]
                │
                ▼
 [ Spawn Main Task on HP Core ]
                │
                ▼
       [ main.c: app_main() ]
                │
                ▼
 [ bios_init.c: bios_core_start() ]
```

Once `app_main()` is active, it immediately relinquishes control to the primary BIOS initialization vector:

```c
void app_main(void)
{
    bios_core_start();
}

This direct handover shifts execution to the BIOS Core dispatch state machine
(defined in bios_init.c), which handles custom wakeups, POST routines, and
payload execution.

2. Design Stability & Decoupling

  - Minimal Execution Overhead: The main file is stripped of all background
    processing, peripheral drivers, and logical loops. This design isolates the
    initial boot vector, ensuring that any crash, watchdogs, or memory
    corruptions are handled entirely inside modular layers (such as the
    Management Engine or the Boot Manager) rather than halting the primary entry
    pipeline.
  - Removal of Legacy Diagnostics: All stress-testing modules, sandbox loops,
    and mock tasks have been decoupled and removed. This ensures the BIOS starts
    in a completely clean, stable, and deterministic state.

3. API Reference

3.1 Primary Application Entry Vector

void app_main(void);

The standard entry point registered by the ESP-IDF build system.

  - Execution Context: Invoked under a dedicated FreeRTOS thread (main task)
    running on the High-Performance core.
  - Action: Instantly calls bios_core_start() to initiate the 3rd-stage BIOS
    bootloader sequence.

---
