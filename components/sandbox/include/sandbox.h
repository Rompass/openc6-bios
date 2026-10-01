#ifndef OPENC6_SANDBOX_H
#define OPENC6_SANDBOX_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "openc6_abi.h"

#ifdef __cplusplus
extern "C" {
    #endif

    /**
     * Minimum DRAM reserved strictly for the kernel during dynamic arena creation.
     * ESP-IDF Wi-Fi station mode requires at least 80-90 KB of free internal heap
     * for net80211/PP buffers, TX/RX descriptors, and LwIP sockets.
     */
    #define SANDBOX_DEFAULT_KERNEL_RESERVE   (72UL * 1024UL)

    /**
     * Dedicated Machine-mode stack size allocated for trap handling.
     */
    #define SANDBOX_STACK_SIZE               (4UL * 1024UL)

    /**
     * @brief Execution status codes returned by the sandbox execution engine.
     */
    typedef enum {
        SANDBOX_STATUS_OK                 = 0,  /**< Payload completed execution normally */
        SANDBOX_STATUS_REQ_WIFI_CONNECT   = 1,  /**< Payload yielded control requesting kernel Wi-Fi service */
        SANDBOX_STATUS_ERR_SIZE           = -1, /**< Binary image exceeds available arena boundary */
        SANDBOX_STATUS_ERR_FAULT          = -2, /**< RISC-V PMP access violation or memory fault */
        SANDBOX_STATUS_ERR_INVALID_OP     = -3, /**< Illegal instruction or unhandled trap */
        SANDBOX_STATUS_ERR_NO_MEM         = -4  /**< Insufficient heap available for arena allocation */
    } sandbox_status_t;

    /**
     * @brief Complete register frame preserved during U-mode trap entry.
     * Matches the exact 144-byte stack layout constructed by sandbox_trap_vector.
     */
    typedef struct {
        uint32_t ra;
        uint32_t sp;
        uint32_t gp;
        uint32_t tp;
        uint32_t t0, t1, t2;
        uint32_t s0, s1;
        uint32_t a0, a1, a2, a3, a4, a5, a6, a7;
        uint32_t s2, s3, s4, s5, s6, s7, s8, s9, s10, s11;
        uint32_t t3, t4, t5, t6;
        uint32_t mepc;
        uint32_t mcause;
        uint32_t mtval;
    } sandbox_trap_frame_t;

    /**
     * @brief Dynamically allocates and configures the isolated sandbox memory window.
     *
     * @param abi Pointer to the host system ABI dispatch table.
     * @param kernel_reserve_bytes DRAM bytes preserved for host RTOS and network stacks.
     * @return true on successful allocation and PMP configuration, false on OOM.
     */
    bool sandbox_init_dynamic(const openc6_abi_t *abi, size_t kernel_reserve_bytes);

    /**
     * @brief Teardowns active PMP registers and releases arena DRAM back to FreeRTOS heap.
     */
    void sandbox_deinit(void);

    /**
     * @brief Retrieves base pointer to the active sandbox arena.
     */
    uint8_t* sandbox_get_arena(void);

    /**
     * @brief Retrieves size in bytes of the active sandbox arena.
     */
    size_t sandbox_get_arena_size(void);

    /**
     * @brief Returns active ABI jump-table instance.
     */
    const openc6_abi_t *sandbox_get_abi(void);

    /**
     * @brief Loads binary image into arena and resets user heap break pointer.
     */
    bool sandbox_load_payload(const uint8_t *src, size_t len);

    /**
     * @brief Enters isolated U-mode execution loop.
     * Automatically services cooperative syscall delegations (e.g. Wi-Fi) by
     * temporarily yielding back to kernel task mode before resuming payload.
     *
     * @return Final exit status of the payload session.
     */
    sandbox_status_t sandbox_run(void);

    /**
     * @brief Allocates dynamic memory within the sandboxed arena boundaries.
     */
    void* sandbox_alloc(uint32_t size);

    /**
     * @brief Releases memory previously allocated via sandbox_alloc.
     */
    void sandbox_free_mem(void *ptr);

    /**
     * @brief Checks if U-mode sandbox execution context is currently active.
     */
    bool sandbox_is_running(void);

    /**
     * @brief Executes a sandboxed U-mode payload in a designated memory slot.
     * Provisions task-local trap frame recovery to allow concurrent multi-process execution.
     *
     * @param entry Physical entry point address for the payload slot.
     * @param user_sp Initial top-of-stack address allocated for the process slot.
     * @return Execution exit status code.
     */
    sandbox_status_t sandbox_run_slot(uintptr_t entry, uintptr_t user_sp, size_t bin_size);

    /**
     * @brief Forces immediate exit from sandbox execution back to host shell.
     */
    void sandbox_request_exit(sandbox_status_t status) __attribute__((noreturn));

    /**
     * @brief Low-level assembly context restore routine.
     * Restores user-space registers, sets MPP=0 (User Mode), and executes mret.
     *
     * @param frame Pointer to preserved register state frame.
     */
    void sandbox_restore_and_mret(const sandbox_trap_frame_t *frame) __attribute__((noreturn));

    #ifdef __cplusplus
}
#endif

#endif /* OPENC6_SANDBOX_H */
