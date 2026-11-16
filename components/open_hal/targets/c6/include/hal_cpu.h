#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "soc/soc.h"

/* RISC-V Privileged Architecture v1.12: Machine-Mode CSRs */
#define HAL_CSR_MSTATUS          0x300
#define HAL_CSR_MIE              0x304
#define HAL_CSR_MTVEC            0x305
#define HAL_CSR_MSCRATCH         0x340
#define HAL_CSR_MEPC             0x341
#define HAL_CSR_MCAUSE           0x342
#define HAL_CSR_MTVAL            0x343

/* ESP-IDF Linker symbol for RISC-V global pointer */
extern int __global_pointer$;

static inline __attribute__((always_inline)) uint32_t hal_cpu_irq_disable(void)
{
    uint32_t old_mstatus;
    asm volatile ("csrrci %0, mstatus, 8" : "=r"(old_mstatus));
    return old_mstatus;
}

static inline __attribute__((always_inline)) void hal_cpu_irq_restore(uint32_t old_mstatus)
{
    asm volatile ("csrw mstatus, %0" : : "r"(old_mstatus));
}

static inline __attribute__((always_inline)) void hal_cpu_irq_enable(void)
{
    asm volatile ("csrs mstatus, 8");
}

static inline __attribute__((always_inline)) void hal_cpu_irq_disable_only(void)
{
    asm volatile ("csrc mstatus, 8");
}

static inline __attribute__((always_inline)) uint32_t hal_cpu_read_mcause(void)
{
    uint32_t val;
    asm volatile ("csrr %0, 0x342" : "=r"(val));
    return val;
}

static inline __attribute__((always_inline)) uint32_t hal_cpu_read_mepc(void)
{
    uint32_t val;
    asm volatile ("csrr %0, 0x341" : "=r"(val));
    return val;
}

static inline __attribute__((always_inline)) uint32_t hal_cpu_read_mtval(void)
{
    uint32_t val;
    asm volatile ("csrr %0, 0x343" : "=r"(val));
    return val;
}

static inline __attribute__((always_inline)) uint32_t hal_cpu_read_ra(void)
{
    uint32_t val;
    asm volatile ("mv %0, ra" : "=r"(val));
    return val;
}

static inline __attribute__((always_inline)) uint32_t hal_cpu_read_sp(void)
{
    uint32_t val;
    asm volatile ("mv %0, sp" : "=r"(val));
    return val;
}

static inline __attribute__((always_inline)) uint32_t hal_cpu_read_mie(void)
{
    uint32_t val;
    asm volatile ("csrr %0, mie" : "=r"(val));
    return val;
}

static inline __attribute__((always_inline)) void hal_cpu_write_mie(uint32_t val)
{
    asm volatile ("csrw mie, %0" : : "r"(val));
}

static inline __attribute__((always_inline)) uint32_t hal_cpu_read_mscratch(void)
{
    uint32_t val;
    asm volatile ("csrr %0, mscratch" : "=r"(val));
    return val;
}

static inline __attribute__((always_inline)) void hal_cpu_write_mscratch(uint32_t val)
{
    asm volatile ("csrw mscratch, %0" : : "r"(val));
}

static inline __attribute__((always_inline)) void hal_cpu_write_mtvec(uint32_t val)
{
    asm volatile ("csrw mtvec, %0" : : "r"(val));
}

static inline __attribute__((always_inline)) uint32_t hal_cpu_swap_mtvec_vectored(uintptr_t table_addr)
{
    uint32_t old_mtvec;
    uintptr_t vec_table = (table_addr & ~0xFFUL) | 0x01UL;
    asm volatile ("csrrw %0, mtvec, %1" : "=r"(old_mtvec) : "r"(vec_table));
    return old_mtvec;
}

static inline __attribute__((always_inline)) void hal_cpu_clear_mip(void)
{
    asm volatile ("csrw mip, zero");
}

static inline __attribute__((always_inline)) void hal_cpu_restore_gp(void)
{
    asm volatile (
        ".option push\n"
        ".option norelax\n"
        "la gp, __global_pointer$\n"
        ".option pop\n"
        : : : "gp"
    );
}

static inline __attribute__((always_inline)) void hal_cpu_force_mmode_mpp(void)
{
    uint32_t mstatus_val;
    asm volatile ("csrr %0, mstatus" : "=r"(mstatus_val));
    mstatus_val |= (3UL << 11);
    asm volatile ("csrw mstatus, %0" : : "r"(mstatus_val));
}

static inline __attribute__((always_inline)) void hal_cpu_set_mret_mode(bool to_umode)
{
    uint32_t mstatus_val;
    asm volatile ("csrr %0, mstatus" : "=r"(mstatus_val));
    if (to_umode) {
        mstatus_val &= ~(3UL << 11); /* Target: User Mode (U-Mode) */
    } else {
        mstatus_val |= (3UL << 11);  /* Target: Machine Mode */
    }
    mstatus_val |= (1UL << 7);       /* MPIE = 1 */
    asm volatile ("csrw mstatus, %0" : : "r"(mstatus_val));
}

static inline __attribute__((always_inline)) void hal_cpu_enter_umode(
    uintptr_t entry, uintptr_t user_sp, uintptr_t arg0, uintptr_t kernel_trap_sp)
{
    /* Keep interrupts masked while configuring MPP=0 and per-task mscratch; mret re-enables via MPIE */
    asm volatile ("csrci mstatus, 8" ::: "memory");

    uint32_t mstatus_val;
    asm volatile ("csrr %0, mstatus" : "=r"(mstatus_val));
    mstatus_val &= ~(3UL << 11);
    mstatus_val |= (1UL << 7);
    asm volatile ("csrw mstatus, %0" : : "r"(mstatus_val));

    asm volatile (
        "csrw 0x340, %3\n"
        "csrw 0x341, %0\n"
        "mv sp, %1\n"
        "mv a0, %2\n"
        "fence.i\n"
        "mret\n"
        : : "r"(entry), "r"(user_sp), "r"(arg0), "r"(kernel_trap_sp)
        : "memory"
    );
}

static inline bool hal_cpu_is_rodata_addr(uintptr_t addr)
{
    return (addr >= SOC_IROM_LOW && addr < SOC_IROM_HIGH);
}

static inline bool hal_cpu_is_rodata_range(uintptr_t addr, size_t len)
{
    return (addr >= SOC_IROM_LOW && addr <= SOC_IROM_HIGH && len <= (SOC_IROM_HIGH - addr));
}
