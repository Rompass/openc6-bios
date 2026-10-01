#include "sandbox.h"
#include "hw_usb.h"
#include "soc/soc.h"

/* RISC-V Privileged Architecture v1.12: PMP CSRs on RV32
 * pmpcfg1  (0x3A1) -> Entries 4..7
 * pmpaddr4 (0x3B4) -> [0 .. arena_start)               (NONE) -> Protect Kernel SRAM (e.g. 0x40800000)
 * pmpaddr5 (0x3B5) -> [arena_start .. arena_end)       (RWX)  -> User sandbox arena (Code, Stack, Heap)
 * pmpaddr6 (0x3B6) -> [arena_end .. SOC_IROM_LOW)      (NONE) -> Protect Upper Kernel SRAM (Wi-Fi heap, RTOS)
 * pmpaddr7 (0x3B7) -> [SOC_IROM_LOW .. SOC_IROM_HIGH)  (R-X)  -> Flash IROM/DROM (ABI ecall trampolines & constants)
 * Entries >= 8: OFF (Default Deny for MMIO peripherals 0x60000000+)
 */
#define PMP_R         (1UL << 0)
#define PMP_W         (1UL << 1)
#define PMP_X         (1UL << 2)
#define PMP_A_TOR     (1UL << 3)

void sandbox_pmp_setup(uintptr_t arena_start, uintptr_t arena_end)
{
    if (arena_start == 0 && arena_end == 0) {
        asm volatile (
            "csrw 0x3a0, zero\n"
            "csrw 0x3a1, zero\n"
            "csrw 0x3b4, zero\n"
            "csrw 0x3b5, zero\n"
            "csrw 0x3b6, zero\n"
            "csrw 0x3b7, zero\n"
            "fence.i\n"
            ::: "memory"
        );
        return;
    }

    /* Ensure Entries 0..3 are disabled and do not interfere */
    asm volatile ("csrw 0x3a0, zero\n csrw 0x3b3, zero\n" ::: "memory");

    uint32_t addr4 = (uint32_t)(arena_start >> 2);
    uint32_t addr5 = (uint32_t)(arena_end >> 2);
    uint32_t addr6 = (uint32_t)(SOC_IROM_LOW >> 2);   // 0x10800000
    uint32_t addr7 = (uint32_t)(SOC_IROM_HIGH >> 2);  // 0x10C00000

    /* Entry 4: TOR, NONE (0x08)
     * Entry 5: TOR, RWX  (0x0F)
     * Entry 6: TOR, NONE (0x08)
     * Entry 7: TOR, R-X  (0x0D)
     * Evaluates to 0x0D080F08 on RV32
     */
    uint32_t pmpcfg1_val = (0x08UL) | (0x0FUL << 8) | (0x08UL << 16) | (0x0DUL << 24);

    asm volatile (
        "csrw 0x3b4, %0\n"
        "csrw 0x3b5, %1\n"
        "csrw 0x3b6, %2\n"
        "csrw 0x3b7, %3\n"
        "csrw 0x3a1, %4\n"
        "fence.i\n"
        :
        : "r"(addr4), "r"(addr5), "r"(addr6), "r"(addr7), "r"(pmpcfg1_val)
        : "memory"
    );
}
