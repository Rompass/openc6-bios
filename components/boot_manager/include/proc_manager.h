#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sandbox.h"
#include "openc6_abi.h"

#ifdef __cplusplus
extern "C" {
    #endif

    /** Maximum concurrent processes tracked in the process table */
    #define OPENC6_MAX_PROCS            8

    /** Allocation page granularity (4 KB pages matching MMU/SRAM architecture) */
    #define OPENC6_PAGE_SIZE            (4UL * 1024UL)

    /** Default user stack size reserved at the top of each process memory space */
    #define OPENC6_PROC_DEFAULT_STACK   (6UL * 1024UL)

    /** Minimum heap reservation when auto-calculating process allocation size */
    #define OPENC6_PROC_MIN_HEAP        (4UL * 1024UL)

    /** ASCII control character for interactive process suspension (Ctrl+X) */
    #define ASCII_CTRL_X                0x18

    /**
     * @brief Process execution lifecycle states.
     */
    typedef enum {
        PROC_STATE_FREE = 0,    /**< Process descriptor slot is vacant */
        PROC_STATE_RUNNING_FG,  /**< Process is actively executing in foreground, blocking console */
        PROC_STATE_RUNNING_BG,  /**< Process is executing concurrently in background */
        PROC_STATE_SUSPENDED,   /**< Execution frozen via signal, retaining allocated memory */
        PROC_STATE_SWAPPED,     /**< Execution frozen, arena pages compressed into ZSWAP buffer */
        PROC_STATE_ZOMBIE       /**< Process terminated, awaiting final resource reclamation */
    } proc_state_t;

    /**
     * @brief Process Control Block (PCB).
     * Tracks metadata, FreeRTOS task references, and dynamically assigned page boundaries.
     */
    typedef struct {
        uint16_t         pid;            /**< Unique process identifier */
        char             name[20];       /**< Process executable name */
        proc_state_t     state;          /**< Current execution lifecycle state */
        TaskHandle_t     task_handle;    /**< Underlying FreeRTOS worker task handle */
        uintptr_t        mem_base;       /**< Start of contiguous dynamically allocated memory block */
        size_t           mem_size;       /**< Total memory capacity allocated to process (multiple of 4KB) */
        uint8_t         *swapped_data;   /**< Pointer to heap-backed ZSWAP compressed buffer (if swapped) */
        size_t           swapped_size;   /**< Byte size of ZSWAP compressed image */
        size_t           bin_size;       /**< Actual executable image byte size */
        uint32_t         start_time_ms;  /**< Timestamp when process was spawned */
        uint32_t         cpu_usage_pct;  /**< Sampled CPU utilization percentage */
        sandbox_status_t exit_code;      /**< Termination exit code or hardware trap status */
    } openc6_proc_t;

    /**
     * @brief Dynamic process memory pool telemetry snapshot.
     */
    typedef struct {
        size_t total_bytes;         /**< Total arena capacity under management */
        size_t used_bytes;          /**< Bytes currently allocated to active processes */
        size_t free_bytes;          /**< Available unallocated bytes */
        size_t largest_free_block;  /**< Maximum contiguous allocation currently possible */
        uint32_t total_pages;       /**< Total 4 KB pages in pool */
        uint32_t free_pages;        /**< Unallocated 4 KB pages */
    } proc_mem_stats_t;

    /**
     * @brief Initializes the OpenC6 Process and Dynamic Memory Subsystem.
     * Sets up process control table, mutex locks, and initializes the page allocation bitmap.
     *
     * @param abi Pointer to global BIOS ABI dispatch jump-table.
     * @return ESP_OK on success, or error code on resource failure.
     */
    esp_err_t proc_manager_init(const openc6_abi_t *abi);

    /**
     * @brief Spawns an executable with dynamically auto-allocated memory pages.
     * Automatically calculates memory needed (binary size + default stack + minimum heap).
     *
     * @param path Filesystem path to the payload binary.
     * @param current_dir Sector ID of current working directory.
     * @param background true to run concurrently in background, false for foreground.
     * @param out_pid Optional pointer to store the newly assigned Process ID.
     * @return ESP_OK on success, ESP_ERR_NO_MEM if contiguous pages unavailable, or error code.
     */
    esp_err_t proc_spawn(const char *path, uint16_t current_dir, bool background, uint16_t *out_pid);

    /**
     * @brief Spawns an executable with custom extra heap padding.
     *
     * @param path Filesystem path to the payload binary.
     * @param current_dir Sector ID of current working directory.
     * @param background true to run concurrently in background, false for foreground.
     * @param extra_heap_bytes Additional heap padding beyond the default stack.
     * @param out_pid Optional pointer to store the newly assigned Process ID.
     * @return ESP_OK on success, ESP_ERR_NO_MEM if contiguous pages unavailable, or error code.
     */
    esp_err_t proc_spawn_mem(const char *path, uint16_t current_dir, bool background,
                             size_t extra_heap_bytes, uint16_t *out_pid);

    /**
     * @brief Forcibly terminates an active or suspended process.
     * Deletes FreeRTOS worker task, returns dynamically allocated pages to the pool,
     * and clears process table descriptor.
     *
     * @param pid Identifier of the process to terminate.
     * @return ESP_OK on success, or ESP_ERR_NOT_FOUND if PID is invalid.
     */
    esp_err_t proc_kill(uint16_t pid);

    /**
     * @brief Suspends the foreground process (Ctrl+X signal handler).
     * Freezes task execution via FreeRTOS scheduler, preserves allocated pages, and yields console.
     *
     * @return ESP_OK on success, or ESP_ERR_INVALID_STATE if no foreground process is running.
     */
    esp_err_t proc_suspend_fg(void);

    /**
     * @brief Resumes a suspended process in the background.
     *
     * @param pid Process ID to resume.
     * @return ESP_OK on success, or error code if PID is invalid or not suspended.
     */
    esp_err_t proc_resume_bg(uint16_t pid);

    /**
     * @brief Brings a background or suspended process to the foreground.
     * Reconnects terminal console stream and blocks until process yields or exits.
     *
     * @param pid Process ID to bring to foreground.
     * @return ESP_OK on success, or error code if PID is invalid.
     */
    esp_err_t proc_resume_fg(uint16_t pid);

    /**
     * @brief Outputs formatted process and dynamic page allocator telemetry (top command).
     */
    void proc_print_top(void);

    /**
     * @brief Queries dynamic arena page allocator metrics.
     *
     * @param stats Pointer to destination statistics structure.
     * @return ESP_OK on success.
     */
    esp_err_t proc_get_mem_stats(proc_mem_stats_t *stats);

    /**
     * @brief Allocates contiguous 4 KB pages from the dynamic sandbox arena.
     *
     * @param size Number of bytes requested (automatically rounded up to 4 KB pages).
     * @return Pointer to base of contiguous memory block, or NULL if insufficient contiguous pages.
     */
    void* proc_mempool_alloc(size_t size);

    /**
     * @brief Releases previously allocated contiguous pages back to the arena pool.
     * Automatically coalesces adjacent free pages.
     *
     * @param ptr Base pointer previously returned by proc_mempool_alloc.
     * @param size Byte size of the allocated region to reclaim.
     */
    void proc_mempool_free(void *ptr, size_t size);

    /**
     * @brief Retrieves the Process ID of the active foreground task.
     *
     * @return Foreground PID, or 0 if shell currently holds console.
     */
    uint16_t proc_get_fg_pid(void);

    /**
     * @brief Checks if a foreground payload is currently executing.
     *
     * @return true if foreground payload active, false otherwise.
     */
    bool proc_is_fg_active(void);

    /**
     * @brief Suspends an active foreground or background process by PID.
     * Freezes task execution via FreeRTOS scheduler, preserves allocated pages.
     *
     * @param pid Process ID to freeze.
     * @return ESP_OK on success, or ESP_ERR_NOT_FOUND if PID is invalid or not running.
     */
    esp_err_t proc_suspend(uint16_t pid);

    void proc_kill_all(void);

    #ifdef __cplusplus
}
#endif
