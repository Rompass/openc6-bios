/* ESP-IDF: Exception and interrupt vectors must reside in internal SRAM (IRAM) */
.section .iram1.text

/* External FreeRTOS handlers from ESP-IDF vectors.S */
.global _interrupt_handler
.global _panic_handler

/*
 * ============================================================================
 * OpenC6 Vectored Trap Table
 * ESP32-C6 TRM: CPU requires 256-byte alignment as it masks lower 8 bits
 * (MTVEC & 0xffffff00).
 * In Vectored Mode (mtvec bit 0 = 1):
 *   - Synchronous exceptions jump to BASE + 0 (Slot 0).
 *   - Asynchronous interrupts jump to BASE + 4 * mcause (Slots 1..31).
 * ============================================================================
 */
.balign 0x100
.option push
.option norvc

.global sandbox_vector_table
.type sandbox_vector_table, @function

sandbox_vector_table:
    j sandbox_trap_vector           /* 0: Exceptions (ecall, PMP faults, illegal instr) */
    j sandbox_intr_trampoline       /* 1: Free interrupt number (Wi-Fi, Timers, etc.) */
    j sandbox_intr_trampoline       /* 2: Free interrupt number */
    j sandbox_intr_trampoline       /* 3: Free interrupt number */
    j sandbox_intr_trampoline       /* 4: Free interrupt number */
    j sandbox_intr_trampoline       /* 5: Free interrupt number */
    j sandbox_intr_trampoline       /* 6: Free interrupt number */
    j sandbox_intr_trampoline       /* 7: Free interrupt number */
    j sandbox_intr_trampoline       /* 8: Free interrupt number */
    j sandbox_intr_trampoline       /* 9: Free interrupt number */
    j sandbox_intr_trampoline       /* 10: Free interrupt number */
    j sandbox_intr_trampoline       /* 11: Free interrupt number */
    j sandbox_intr_trampoline       /* 12: Free interrupt number */
    j sandbox_intr_trampoline       /* 13: Free interrupt number */
    j sandbox_intr_trampoline       /* 14: Free interrupt number */
    j sandbox_intr_trampoline       /* 15: Free interrupt number */
    j sandbox_intr_trampoline       /* 16: Free interrupt number */
    j sandbox_intr_trampoline       /* 17: Free interrupt number */
    j sandbox_intr_trampoline       /* 18: Free interrupt number */
    j sandbox_intr_trampoline       /* 19: Free interrupt number */
    j sandbox_intr_trampoline       /* 20: Free interrupt number */
    j sandbox_intr_trampoline       /* 21: Free interrupt number */
    j sandbox_intr_trampoline       /* 22: Free interrupt number */
    j sandbox_intr_trampoline       /* 23: Free interrupt number */
    j _panic_handler                /* 24: ETS_INT_WDT_INUM (Hardware Watchdog) */
    j _panic_handler                /* 25: ETS_CACHEERR_INUM (Cache error) */
    j sandbox_intr_trampoline       /* 26: Free interrupt number */
    j sandbox_intr_trampoline       /* 27: ASTDBG_ISR (Assist Debug) */
    j sandbox_intr_trampoline       /* 28: Free interrupt number */
    j sandbox_intr_trampoline       /* 29: Free interrupt number */
    j sandbox_intr_trampoline       /* 30: Free interrupt number */
    j sandbox_intr_trampoline       /* 31: Free interrupt number */

.size sandbox_vector_table, .-sandbox_vector_table
.option pop

/*
 * ============================================================================
 * Asynchronous Interrupt Trampoline (Slots 1..23, 26..31)
 *
 * @brief Intercepts hardware interrupts to enforce privilege level hygiene.
 * FreeRTOS RISC-V port assumes all tasks operate strictly in Machine Mode (M-Mode)
 * and passes the interrupted mstatus.MPP through context switches without per-task
 * tracking. When an interrupt hits a sandboxed U-mode payload, mstatus.MPP is 00.
 * If FreeRTOS subsequently switches context to a kernel task (e.g. vPortYield),
 * mret drops the kernel task into U-mode at 0x4080ba8c, triggering an immediate
 * PMP Instruction Access Fault.
 *
 * This trampoline enforces:
 * 1. M-mode interrupts pass directly to ESP-IDF's _interrupt_handler.
 * 2. U-mode interrupts have their original PC staged, mepc pointed to a return
 *    trampoline, and mstatus.MPP elevated to 3 before handing off to FreeRTOS.
 * ============================================================================
 */
.balign 4
.option push
.option norvc

.global sandbox_intr_trampoline
.type sandbox_intr_trampoline, @function

sandbox_intr_trampoline:
    /* Allocate 16 bytes on active SP to stage inspection registers */
    addi sp, sp, -16
    sw   t0, 0(sp)
    sw   t1, 4(sp)

    /* Inspect privilege mode of interrupted context: mstatus.MPP (bits 11..12) */
    csrr t0, mstatus
    li   t1, (3 << 11)
    and  t1, t0, t1
    bnez t1, .Lintr_from_mmode

    /* ------------------------------------------------------------------------
     * Interrupted from User Mode (U-Mode, MPP == 0)
     * ------------------------------------------------------------------------ */
    /* Preserve original U-mode execution PC inside trampoline frame */
    csrr t0, mepc
    sw   t0, 8(sp)

    /* Divert mepc to the U-mode return trampoline so FreeRTOS returns there */
    la   t0, sandbox_umode_return_trampoline
    csrw mepc, t0

    /* Elevate mstatus.MPP to Machine Mode (3) so FreeRTOS scheduler stays safe */
    csrr t0, mstatus
    li   t1, (3 << 11)
    or   t0, t0, t1
    csrw mstatus, t0

    /* Restore working registers, keeping the 16-byte trampoline frame on stack */
    lw   t0, 0(sp)
    lw   t1, 4(sp)
    j    _interrupt_handler

.Lintr_from_mmode:
    /* ------------------------------------------------------------------------
     * Interrupted from Machine Mode (M-Mode, MPP == 3)
     * ------------------------------------------------------------------------ */
    /* Native kernel execution: restore registers, unwind stack, invoke IDF handler */
    lw   t0, 0(sp)
    lw   t1, 4(sp)
    addi sp, sp, 16
    j    _interrupt_handler

.size sandbox_intr_trampoline, .-sandbox_intr_trampoline

/*
 * ============================================================================
 * User Mode Safe Return Trampoline
 *
 * @brief Restores unprivileged U-mode privilege state after FreeRTOS interrupt
 * processing completes for a sandboxed task.
 * ============================================================================
 */
.balign 4
.global sandbox_umode_return_trampoline
.type sandbox_umode_return_trampoline, @function

sandbox_umode_return_trampoline:
    /* Atomically disable interrupts during final privilege demotion */
    csrci mstatus, 8

    /* Save scratch registers in trampoline frame to safely manipulate CSRs */
    sw   t0, 0(sp)
    sw   t1, 4(sp)

    /* Restore original U-mode execution PC from frame */
    lw   t0, 8(sp)
    csrw mepc, t0

    /* Demote execution mode to User Mode (MPP = 00) and arm MPIE */
    csrr t0, mstatus
    li   t1, ~(3 << 11)
    and  t0, t0, t1
    ori  t0, t0, (1 << 7)
    csrw mstatus, t0

    /* Restore scratch registers and pop 16-byte trampoline frame */
    lw   t0, 0(sp)
    lw   t1, 4(sp)
    addi sp, sp, 16

    /* Return to sandboxed User Mode payload */
    mret

.size sandbox_umode_return_trampoline, .-sandbox_umode_return_trampoline
.option pop

/*
 * ============================================================================
 * Synchronous Trap Handler (Slot 0)
 * ============================================================================
 */
.balign 4
.option push
.option norvc

.global sandbox_trap_vector
.type sandbox_trap_vector, @function

sandbox_trap_vector:
    /* RISC-V Privileged Arch v1.12: Atomically swap untrusted user SP with pre-loaded kernel SP in mscratch */
    csrrw sp, 0x340, sp

    /* Allocate 144-byte trap frame (aligned to 16 bytes per RV32 calling convention) */
    addi sp, sp, -144

    sw x1,   0(sp)
    /* Read untrusted user SP from mscratch and preserve it inside the frame */
    csrr x1, 0x340
    sw x1,   4(sp)

    sw x3,   8(sp)
    sw x4,  12(sp)
    sw x5,  16(sp)
    sw x6,  20(sp)
    sw x7,  24(sp)
    sw x8,  28(sp)
    sw x9,  32(sp)
    sw x10, 36(sp)
    sw x11, 40(sp)
    sw x12, 44(sp)
    sw x13, 48(sp)
    sw x14, 52(sp)
    sw x15, 56(sp)
    sw x16, 60(sp)
    sw x17, 64(sp)
    sw x18, 68(sp)
    sw x19, 72(sp)
    sw x20, 76(sp)
    sw x21, 80(sp)
    sw x22, 84(sp)
    sw x23, 88(sp)
    sw x24, 92(sp)
    sw x25, 96(sp)
    sw x26, 100(sp)
    sw x27, 104(sp)
    sw x28, 108(sp)
    sw x29, 112(sp)
    sw x30, 116(sp)
    sw x31, 120(sp)

    /* Preserve diagnostic CSRs: mepc (0x341), mcause (0x342), mtval (0x343) */
    csrr t0, 0x341
    csrr t1, 0x342
    csrr t2, 0x343
    sw t0, 124(sp)
    sw t1, 128(sp)
    sw t2, 132(sp)

    /* Dispatch to C handler: a0 = frame, a1 = mcause, a2 = mtval */
    mv a0, sp
    mv a1, t1
    mv a2, t2
    call sandbox_dispatch_trap

    /* Reload mepc in case handler modified it (e.g. advanced past ecall) */
    lw t0, 124(sp)
    csrw 0x341, t0

    /* Restore general-purpose registers */
    lw x1,   0(sp)
    lw x3,   8(sp)
    lw x4,  12(sp)
    lw x5,  16(sp)
    lw x6,  20(sp)
    lw x7,  24(sp)
    lw x8,  28(sp)
    lw x9,  32(sp)
    lw x10, 36(sp)
    lw x11, 40(sp)
    lw x12, 44(sp)
    lw x13, 48(sp)
    lw x14, 52(sp)
    lw x15, 56(sp)
    lw x16, 60(sp)
    lw x17, 64(sp)
    lw x18, 68(sp)
    lw x19, 72(sp)
    lw x20, 76(sp)
    lw x21, 80(sp)
    lw x22, 84(sp)
    lw x23, 88(sp)
    lw x24, 92(sp)
    lw x25, 96(sp)
    lw x26, 100(sp)
    lw x27, 104(sp)
    lw x28, 108(sp)
    lw x29, 112(sp)
    lw x30, 116(sp)
    lw x31, 120(sp)

    /* Stage user stack pointer into mscratch prior to final atomic register swap */
    lw t0, 4(sp)
    csrw 0x340, t0

    addi sp, sp, 144
    csrrw sp, 0x340, sp
    mret

.size sandbox_trap_vector, .-sandbox_trap_vector
.option pop

/*
 * ============================================================================
 * Context Restore and Return to U-mode via mret
 * ============================================================================
 */
.balign 4
.option push
.option norvc

.global sandbox_restore_and_mret
.type sandbox_restore_and_mret, @function

sandbox_restore_and_mret:
    /* Stage host kernel stack into mscratch */
    csrw 0x340, sp

    /* Restore target execution PC */
    lw t0, 124(a0)
    csrw 0x341, t0

    /* Restore GPRs */
    lw x1,   0(a0)
    lw x3,   8(a0)
    lw x4,  12(a0)
    lw x5,  16(a0)
    lw x6,  20(a0)
    lw x7,  24(a0)
    lw x8,  28(a0)
    lw x9,  32(a0)
    lw x11, 40(a0)
    lw x12, 44(a0)
    lw x13, 48(a0)
    lw x14, 52(a0)
    lw x15, 56(a0)
    lw x16, 60(a0)
    lw x17, 64(a0)
    lw x18, 68(a0)
    lw x19, 72(a0)
    lw x20, 76(a0)
    lw x21, 80(a0)
    lw x22, 84(a0)
    lw x23, 88(a0)
    lw x24, 92(a0)
    lw x25, 96(a0)
    lw x26, 100(a0)
    lw x27, 104(a0)
    lw x28, 108(a0)
    lw x29, 112(a0)
    lw x30, 116(a0)
    lw x31, 120(a0)

    /* Restore user stack pointer */
    lw sp, 4(a0)

    /* Restore a0 */
    lw x10, 36(a0)

    fence.i
    mret

.size sandbox_restore_and_mret, .-sandbox_restore_and_mret
.option pop
