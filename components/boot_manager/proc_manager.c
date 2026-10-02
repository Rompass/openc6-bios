#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "proc_manager.h"
#include "sandbox.h"
#include "openc6_fs.h"
#include "hw_usb.h"
#include "led_mgmt.h"
#include "me_shared.h"
#include "nvram.h"
#include "zc6.h"

static const char *TAG __attribute__((unused)) = "PROC_MGR";

/* Maximum pages supported in arena pool (128 pages * 4 KB = 512 KB addressable) */
#define MAX_POOL_PAGES              128U
#define BITMAP_WORDS                ((MAX_POOL_PAGES + 31U) / 32U)

static openc6_proc_t s_procs[OPENC6_MAX_PROCS];
static SemaphoreHandle_t s_proc_mux = NULL;
static SemaphoreHandle_t s_fg_done_sem = NULL;
static uint16_t s_next_pid = 1;
static uint16_t s_fg_pid = 0;
static const openc6_abi_t *s_abi = NULL;

/* Dynamic page allocator tracking state */
static uint32_t s_page_bitmap[BITMAP_WORDS];
static size_t s_total_pages = 0;
static uint8_t *s_pool_base = NULL;
static portMUX_TYPE s_pool_lock = portMUX_INITIALIZER_UNLOCKED;

/**
 * @brief Synchronizes dynamic page pool state with the underlying sandbox arena.
 */
static void proc_mempool_sync(void)
{
    uint8_t *arena = sandbox_get_arena();
    size_t arena_sz = sandbox_get_arena_size();

    if (!arena || arena_sz < OPENC6_PAGE_SIZE) {
        s_pool_base = NULL;
        s_total_pages = 0;
        return;
    }

    if (s_pool_base != arena) {
        portENTER_CRITICAL(&s_pool_lock);
        s_pool_base = arena;
        s_total_pages = arena_sz / OPENC6_PAGE_SIZE;
        if (s_total_pages > MAX_POOL_PAGES) {
            s_total_pages = MAX_POOL_PAGES;
        }
        memset(s_page_bitmap, 0, sizeof(s_page_bitmap));
        portEXIT_CRITICAL(&s_pool_lock);
    }
}

static size_t s_alloc_rover = 0;

void* proc_mempool_alloc(size_t size)
{
    if (size == 0) {
        return NULL;
    }

    proc_mempool_sync();
    if (!s_pool_base || s_total_pages == 0) {
        return NULL;
    }

    size_t pages_needed = (size + OPENC6_PAGE_SIZE - 1U) / OPENC6_PAGE_SIZE;
    if (pages_needed > s_total_pages) {
        return NULL;
    }

    portENTER_CRITICAL(&s_pool_lock);
    int start_page = -1;
    size_t consecutive = 0;

    /* Next-Fit search: circulate through arena to prevent stepping on swapped process regions */
    for (size_t step = 0; step < s_total_pages * 2; step++) {
        size_t p = (s_alloc_rover + step) % s_total_pages;
        uint32_t word_idx = p / 32U;
        uint32_t bit_mask = 1UL << (p % 32U);

        if ((s_page_bitmap[word_idx] & bit_mask) == 0) {
            if (consecutive == 0) {
                start_page = (int)p;
            }
            consecutive++;
            if (consecutive == pages_needed) {
                break;
            }
        } else {
            consecutive = 0;
            start_page = -1;
        }
    }

    if (consecutive != pages_needed || start_page < 0) {
        portEXIT_CRITICAL(&s_pool_lock);
        return NULL;
    }

    /* Mark allocated pages as occupied */
    for (size_t i = 0; i < pages_needed; i++) {
        size_t p = (size_t)start_page + i;
        s_page_bitmap[p / 32U] |= (1UL << (p % 32U));
    }
    s_alloc_rover = ((size_t)start_page + pages_needed) % s_total_pages;
    portEXIT_CRITICAL(&s_pool_lock);

    return (void *)(s_pool_base + ((size_t)start_page * OPENC6_PAGE_SIZE));
}

/**
 * @brief Allocates specific physical pages to restore swapped process without relocation.
 */
static void* proc_mempool_alloc_at(uintptr_t target_base, size_t size)
{
    if (target_base == 0 || size == 0 || !s_pool_base) {
        return NULL;
    }
    if (target_base < (uintptr_t)s_pool_base) {
        return NULL;
    }

    size_t offset = target_base - (uintptr_t)s_pool_base;
    if ((offset % OPENC6_PAGE_SIZE) != 0) {
        return NULL;
    }

    size_t start_page = offset / OPENC6_PAGE_SIZE;
    size_t pages_needed = (size + OPENC6_PAGE_SIZE - 1U) / OPENC6_PAGE_SIZE;

    if (start_page + pages_needed > s_total_pages) {
        return NULL;
    }

    portENTER_CRITICAL(&s_pool_lock);
    /* Verify target pages are completely vacant */
    for (size_t i = 0; i < pages_needed; i++) {
        size_t p = start_page + i;
        uint32_t word_idx = p / 32U;
        uint32_t bit_mask = 1UL << (p % 32U);
        if ((s_page_bitmap[word_idx] & bit_mask) != 0) {
            portEXIT_CRITICAL(&s_pool_lock);
            return NULL; /* Collision: memory occupied by another process */
        }
    }

    /* Claim exact physical pages */
    for (size_t i = 0; i < pages_needed; i++) {
        size_t p = start_page + i;
        s_page_bitmap[p / 32U] |= (1UL << (p % 32U));
    }
    portEXIT_CRITICAL(&s_pool_lock);

    return (void *)target_base;
}

void proc_mempool_free(void *ptr, size_t size)
{
    if (!ptr || !s_pool_base || size == 0) {
        return;
    }

    uintptr_t addr = (uintptr_t)ptr;
    uintptr_t base = (uintptr_t)s_pool_base;

    if (addr < base) {
        return;
    }

    size_t offset = addr - base;
    size_t start_page = offset / OPENC6_PAGE_SIZE;
    size_t pages_to_free = (size + OPENC6_PAGE_SIZE - 1U) / OPENC6_PAGE_SIZE;

    portENTER_CRITICAL(&s_pool_lock);
    for (size_t i = 0; i < pages_to_free; i++) {
        size_t p = start_page + i;
        if (p < s_total_pages) {
            s_page_bitmap[p / 32U] &= ~(1UL << (p % 32U));
        }
    }
    portEXIT_CRITICAL(&s_pool_lock);
}

esp_err_t proc_get_mem_stats(proc_mem_stats_t *stats)
{
    if (!stats) {
        return ESP_ERR_INVALID_ARG;
    }

    proc_mempool_sync();

    portENTER_CRITICAL(&s_pool_lock);
    size_t free_cnt = 0;
    size_t max_consecutive = 0;
    size_t current_consecutive = 0;

    for (size_t p = 0; p < s_total_pages; p++) {
        uint32_t word_idx = p / 32U;
        uint32_t bit_mask = 1UL << (p % 32U);

        if ((s_page_bitmap[word_idx] & bit_mask) == 0) {
            free_cnt++;
            current_consecutive++;
            if (current_consecutive > max_consecutive) {
                max_consecutive = current_consecutive;
            }
        } else {
            current_consecutive = 0;
        }
    }

    stats->total_pages = (uint32_t)s_total_pages;
    stats->free_pages = (uint32_t)free_cnt;
    stats->total_bytes = s_total_pages * OPENC6_PAGE_SIZE;
    stats->free_bytes = free_cnt * OPENC6_PAGE_SIZE;
    stats->used_bytes = stats->total_bytes - stats->free_bytes;
    stats->largest_free_block = max_consecutive * OPENC6_PAGE_SIZE;
    portEXIT_CRITICAL(&s_pool_lock);

    return ESP_OK;
}

/**
 * @brief Resolves relative or absolute path within the virtual filesystem.
 */
static int16_t proc_resolve_path(const char *path, uint16_t current_dir, char *out_name)
{
    uint16_t walk_dir = current_dir;
    char temp[128];
    strncpy(temp, path, 127);
    temp[127] = '\0';

    if (temp[0] == '/') {
        walk_dir = 0;
    }

    char *saveptr = NULL;
    char *token = strtok_r(temp, "/", &saveptr);
    char *last_token = NULL;

    while (token != NULL) {
        if (last_token != NULL) {
            if (strcmp(last_token, "..") == 0) {
                walk_dir = fs_get_parent_id(walk_dir);
            } else if (strcmp(last_token, ".") != 0) {
                int16_t next_id = fs_find_id(last_token, walk_dir);
                if (next_id >= 0 && fs_get_type(next_id) == TYPE_DIR) {
                    walk_dir = (uint16_t)next_id;
                } else {
                    return -1;
                }
            }
        }
        last_token = token;
        token = strtok_r(NULL, "/", &saveptr);
    }

    if (!last_token) {
        out_name[0] = '\0';
        return (int16_t)walk_dir;
    }

    if (strcmp(last_token, "..") == 0) {
        walk_dir = fs_get_parent_id(walk_dir);
        out_name[0] = '\0';
    } else if (strcmp(last_token, ".") == 0) {
        out_name[0] = '\0';
    } else {
        strncpy(out_name, last_token, 17);
        out_name[17] = '\0';
    }
    return (int16_t)walk_dir;
}

/**
 * @brief Dedicated FreeRTOS worker task wrapping U-mode sandbox execution for an allocated memory block.
 */
static void proc_sandbox_task_entry(void *arg)
{
    openc6_proc_t *proc = (openc6_proc_t *)arg;

    /* Extinguish background Aura task so active payloads retain exclusive WS2812 access */
    led_mgmt_set_aura_mode(AURA_DISABLED);
    led_mgmt_set_color(0, 0, 0);

    /* Compute aligned stack top inside process memory space */
    uintptr_t slot_sp = ((proc->mem_base + proc->mem_size) & ~15UL) - 64;

    /* Execute payload from dynamically allocated memory pages */
    proc->exit_code = sandbox_run_slot(proc->mem_base, slot_sp, proc->bin_size);

    char term_msg[80];
    snprintf(term_msg, sizeof(term_msg), "\r\n[PROC %u] Process '%s' exited (status: %d)\r\n",
             proc->pid, proc->name, (int)proc->exit_code);
    hw_usb_print(term_msg);

    xSemaphoreTake(s_proc_mux, portMAX_DELAY);

    /* Release dynamic memory pages back to the arena pool */
    if (proc->mem_base != 0 && proc->mem_size != 0) {
        proc_mempool_free((void *)proc->mem_base, proc->mem_size);
        proc->mem_base = 0;
        proc->mem_size = 0;
    }

    if (s_fg_pid == proc->pid) {
        s_fg_pid = 0;
        if (s_fg_done_sem) {
            xSemaphoreGive(s_fg_done_sem);
        }
    }
    proc->state = PROC_STATE_FREE;
    proc->task_handle = NULL;

    /* Only restore Aura from NVRAM if no other concurrent background payload is active */
    bool any_active = false;
    for (int i = 0; i < OPENC6_MAX_PROCS; i++) {
        if (s_procs[i].state == PROC_STATE_RUNNING_FG || s_procs[i].state == PROC_STATE_RUNNING_BG) {
            any_active = true;
            break;
        }
    }
    if (!any_active) {
        led_mgmt_apply_aura_from_nvram();
    }

    xSemaphoreGive(s_proc_mux);
    vTaskDelete(NULL);
}

/* Forward declaration of non-blocking command getter from c6wsh */
extern bool c6wsh_get_pending_command(char *dest, size_t max_len);

/**
 * @brief Waits for active foreground process to yield or terminate while intercepting Ctrl+X.
 * Concurrently polls physical USB CDC terminal and remote c6wsh web command queue.
 */
static void proc_wait_fg_loop(void)
{
    char web_cmd[64];

    while (proc_is_fg_active()) {
        management_engine_pet_watchdog();

        /* 1. Catch ASCII 0x18 (Ctrl+X) from physical USB CDC terminal */
        while (hw_usb_has_data()) {
            char c = hw_usb_getc();
            if ((uint8_t)c == ASCII_CTRL_X) {
                hw_usb_print("\r\n[Ctrl+X] Suspending foreground process...\r\n");
                proc_suspend_fg();
                return;
            }
        }

        /* 2. Catch remote 'suspend' signal or Ctrl+X from c6wsh Web Shell */
        if (c6wsh_get_pending_command(web_cmd, sizeof(web_cmd))) {
            if (strcmp(web_cmd, "suspend") == 0 ||
                strcmp(web_cmd, "ctrl-x") == 0 ||
                (uint8_t)web_cmd[0] == ASCII_CTRL_X) {
                hw_usb_print("\r\n[c6wsh] Suspending foreground process via remote signal...\r\n");
            proc_suspend_fg();
            return;
                } else {
                    hw_usb_print("\r\n[c6wsh] Foreground job is active. Press 'SUSPEND [Ctrl+X]' first.\r\n");
                }
        }

        /* 3. Check if process finished execution */
        if (xSemaphoreTake(s_fg_done_sem, pdMS_TO_TICKS(20)) == pdTRUE) {
            break;
        }
    }
}

esp_err_t proc_manager_init(const openc6_abi_t *abi)
{
    if (abi != NULL) {
        s_abi = abi;
    }

    if (s_proc_mux == NULL) {
        s_proc_mux = xSemaphoreCreateMutex();
    }
    if (s_fg_done_sem == NULL) {
        s_fg_done_sem = xSemaphoreCreateBinary();
    }

    for (int i = 0; i < OPENC6_MAX_PROCS; i++) {
        s_procs[i].pid = 0;
        s_procs[i].state = PROC_STATE_FREE;
        s_procs[i].task_handle = NULL;
        s_procs[i].mem_base = 0;
        s_procs[i].mem_size = 0;
        s_procs[i].swapped_data = NULL;
        s_procs[i].swapped_size = 0;
        s_procs[i].bin_size = 0;
    }

    proc_mempool_sync();
    return ESP_OK;
}

esp_err_t proc_spawn_mem(const char *path, uint16_t current_dir, bool background,
                         size_t extra_heap_bytes, uint16_t *out_pid)
{
    if (!path || strlen(path) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Locate executable binary on flash filesystem */
    char name[18];
    int16_t p_dir = proc_resolve_path(path, current_dir, name);
    if (p_dir < 0 || strlen(name) == 0) {
        hw_usb_print("Error: Invalid executable path.\r\n");
        return ESP_ERR_NOT_FOUND;
    }

    int16_t file_id = fs_find_id(name, (uint16_t)p_dir);
    if (file_id < 0 || fs_get_type(file_id) != TYPE_FILE) {
        hw_usb_print("Error: Binary file not found.\r\n");
        return ESP_ERR_NOT_FOUND;
    }

    int32_t file_size = fs_get_size(file_id);
    if (file_size <= 0) {
        hw_usb_print("Error: Executable file is empty.\r\n");
        return ESP_ERR_INVALID_SIZE;
    }

    /* 1. Detect if target binary is compressed with ZC6 */
    zc6_swap_mode_t swap_mode = VAL_ZC6_SWAP_DEFAULT;
    nvram_get_zc6_swap_mode(&swap_mode);

    bool is_zc6 = false;
    zc6_header_t zc6_hdr;
    size_t exec_bin_size = (size_t)file_size;

    if (swap_mode == ZC6_SWAP_FLASH || swap_mode == ZC6_SWAP_BOTH) {
        if (file_size >= (int32_t)sizeof(zc6_header_t)) {
            if (fs_read_file((uint16_t)file_id, (uint8_t *)&zc6_hdr, 0, sizeof(zc6_hdr)) == (int32_t)sizeof(zc6_hdr)) {
                if (zc6_hdr.magic == ZC6_MAGIC && zc6_hdr.version == ZC6_VERSION) {
                    is_zc6 = true;
                    exec_bin_size = (size_t)zc6_hdr.raw_size;
                }
            }
        }
    }

    /* Ensure dynamic sandbox arena and hardware PMP window are provisioned */
    if (!sandbox_get_arena()) {
        if (!sandbox_init_dynamic(s_abi, SANDBOX_DEFAULT_KERNEL_RESERVE)) {
            hw_usb_print("Error: Insufficient DRAM for sandbox arena.\r\n");
            return ESP_ERR_NO_MEM;
        }
    }

    /* Dynamically calculate memory needed using restored uncompressed binary size */
    size_t min_needed = exec_bin_size + OPENC6_PROC_DEFAULT_STACK + OPENC6_PROC_MIN_HEAP + extra_heap_bytes;
    size_t alloc_size = (min_needed + OPENC6_PAGE_SIZE - 1U) & ~(OPENC6_PAGE_SIZE - 1U);

    xSemaphoreTake(s_proc_mux, portMAX_DELAY);

    /* Allocate free slot in process table */
    int slot_idx = -1;
    for (int i = 0; i < OPENC6_MAX_PROCS; i++) {
        if (s_procs[i].state == PROC_STATE_FREE) {
            slot_idx = i;
            break;
        }
    }

    if (slot_idx < 0) {
        xSemaphoreGive(s_proc_mux);
        hw_usb_print("Error: Process table full (max 8 concurrent payloads).\r\n");
        return ESP_ERR_NO_MEM;
    }

    /* Allocate contiguous 4 KB pages from dynamic pool */
    uint8_t *slot_base = (uint8_t *)proc_mempool_alloc(alloc_size);
    if (!slot_base) {
        proc_mem_stats_t st;
        proc_get_mem_stats(&st);
        char err[128];
        snprintf(err, sizeof(err),
                 "Error: Insufficient contiguous pages (Need %zu KB, largest free block is %zu KB).\r\n",
                 alloc_size / 1024, st.largest_free_block / 1024);
        hw_usb_print(err);
        xSemaphoreGive(s_proc_mux);
        return ESP_ERR_NO_MEM;
    }

    memset(slot_base, 0, alloc_size);

    /* Stage executable binary into allocated pages (Transparent ZC6 decompression) */
    if (is_zc6) {
        uint8_t *comp_buf = (uint8_t *)malloc((size_t)file_size);
        if (!comp_buf) {
            proc_mempool_free(slot_base, alloc_size);
            xSemaphoreGive(s_proc_mux);
            hw_usb_print("Error: Insufficient DRAM for ZC6 decompression staging.\r\n");
            return ESP_ERR_NO_MEM;
        }

        if (fs_read_file((uint16_t)file_id, comp_buf, 0, (uint32_t)file_size) != file_size) {
            free(comp_buf);
            proc_mempool_free(slot_base, alloc_size);
            xSemaphoreGive(s_proc_mux);
            hw_usb_print("Error: Flash read error on compressed binary.\r\n");
            return ESP_FAIL;
        }

        size_t decomp_len = 0;
        bool ok = zc6_decompress(comp_buf, (size_t)file_size, slot_base, alloc_size, &decomp_len);
        free(comp_buf);

        if (!ok || decomp_len != exec_bin_size) {
            proc_mempool_free(slot_base, alloc_size);
            xSemaphoreGive(s_proc_mux);
            hw_usb_print("Error: ZC6 decompression failed or CRC corruption detected.\r\n");
            return ESP_FAIL;
        }
    } else {
        if (fs_read_file((uint16_t)file_id, slot_base, 0, (uint32_t)file_size) != file_size) {
            proc_mempool_free(slot_base, alloc_size);
            xSemaphoreGive(s_proc_mux);
            hw_usb_print("Error: Flash read error while staging binary into memory.\r\n");
            return ESP_FAIL;
        }
    }

    openc6_proc_t *proc = &s_procs[slot_idx];
    proc->pid = s_next_pid++;
    if (s_next_pid == 0) s_next_pid = 1;
    strncpy(proc->name, name, sizeof(proc->name) - 1);
    proc->name[sizeof(proc->name) - 1] = '\0';
    proc->mem_base = (uintptr_t)slot_base;
    proc->mem_size = alloc_size;
    proc->bin_size = exec_bin_size;
    proc->start_time_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    proc->state = background ? PROC_STATE_RUNNING_BG : PROC_STATE_RUNNING_FG;

    if (!background) {
        s_fg_pid = proc->pid;
    }
    if (out_pid) {
        *out_pid = proc->pid;
    }

    /* Spawn preemptive task with 6KB stack for LwIP socket operations */
    BaseType_t ret = xTaskCreate(proc_sandbox_task_entry, "u_sandbox", 2560, proc, 5, &proc->task_handle);
    xSemaphoreGive(s_proc_mux);

    if (ret != pdPASS) {
        xSemaphoreTake(s_proc_mux, portMAX_DELAY);
        proc_mempool_free((void *)proc->mem_base, proc->mem_size);
        proc->state = PROC_STATE_FREE;
        xSemaphoreGive(s_proc_mux);
        hw_usb_print("Error: Failed to spawn FreeRTOS sandbox task.\r\n");
        return ESP_FAIL;
    }

    size_t page_count = alloc_size / OPENC6_PAGE_SIZE;
    if (background) {
        char msg[160];
        if (is_zc6) {
            snprintf(msg, sizeof(msg), "[JOB %u] Running in background: '%s' (Alloc: %zu KB, ZC6: %ld -> %zu B)\r\n",
                     proc->pid, proc->name, alloc_size / 1024, (long)file_size, exec_bin_size);
        } else {
            snprintf(msg, sizeof(msg), "[JOB %u] Running in background: '%s' (Alloc: %zu KB [%zu pages], Bin: %ld B)\r\n",
                     proc->pid, proc->name, alloc_size / 1024, page_count, (long)file_size);
        }
        hw_usb_print(msg);
    } else {
        char msg[160];
        if (is_zc6) {
            snprintf(msg, sizeof(msg), "[BOOT] Running '%s' (PID: %u, Alloc: %zu KB, ZC6: %ld -> %zu B). Press Ctrl+X to suspend.\r\n",
                     proc->name, proc->pid, alloc_size / 1024, (long)file_size, exec_bin_size);
        } else {
            snprintf(msg, sizeof(msg), "[BOOT] Running '%s' (PID: %u, Alloc: %zu KB [%zu pages]). Press Ctrl+X to suspend.\r\n",
                     proc->name, proc->pid, alloc_size / 1024, page_count);
        }
        hw_usb_print(msg);
        proc_wait_fg_loop();
    }

    return ESP_OK;
}

esp_err_t proc_spawn(const char *path, uint16_t current_dir, bool background, uint16_t *out_pid)
{
    /* Default auto-sizing spawn: allocate exact binary size + stack + standard heap padding */
    return proc_spawn_mem(path, current_dir, background, 0, out_pid);
}

/* External process cleanup hooks in sandbox_core.c */
extern void sandbox_close_task_sockets(TaskHandle_t task);
extern void sandbox_unregister_task_external(TaskHandle_t task);

/**
 * @brief Terminates an active process by PID, safely reclaiming memory and descriptors.
 * Closes network sockets first to release bound ports, deletes the task while host vectors
 * remain protected, and finally restores machine CSR state once the thread is destroyed.
 *
 * @param pid Process identifier.
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if PID does not exist.
 */
esp_err_t proc_kill(uint16_t pid)
{
    xSemaphoreTake(s_proc_mux, portMAX_DELAY);
    openc6_proc_t *target = NULL;
    for (int i = 0; i < OPENC6_MAX_PROCS; i++) {
        if (s_procs[i].pid == pid && s_procs[i].state != PROC_STATE_FREE) {
            target = &s_procs[i];
            break;
        }
    }

    if (!target) {
        xSemaphoreGive(s_proc_mux);
        return ESP_ERR_NOT_FOUND;
    }

    if (target->task_handle != NULL) {
        TaskHandle_t dead_task = target->task_handle;

        /* 1. Remove task from all FreeRTOS queues first to prevent mbox deadlocks */
        vTaskDelete(dead_task);
        target->task_handle = NULL;

        /* 2. Now that no task is waiting on the socket, close it safely to free port 8080 */
        sandbox_close_task_sockets(dead_task);

        /* 3. Unregister process state and restore host MTVEC/MSCRATCH if pool is empty */
        sandbox_unregister_task_external(dead_task);
    }

    /* 4. Immediately reclaim dynamically allocated memory pages */
    if (target->mem_base != 0 && target->mem_size != 0) {
        proc_mempool_free((void *)target->mem_base, target->mem_size);
        target->mem_base = 0;
        target->mem_size = 0;
    }

    /* 5. Reclaim heap-backed ZSWAP memory if process was swapped */
    if (target->swapped_data != NULL) {
        free(target->swapped_data);
        target->swapped_data = NULL;
        target->swapped_size = 0;
    }

    if (s_fg_pid == pid) {
        s_fg_pid = 0;
        if (s_fg_done_sem) {
            xSemaphoreGive(s_fg_done_sem);
        }
    }

    char msg[80];
    snprintf(msg, sizeof(msg), "[JOB %u] Process '%s' killed by signal.\r\n", target->pid, target->name);
    hw_usb_print(msg);

    target->state = PROC_STATE_FREE;

    /* Restore Aura only if all remaining process slots are empty */
    bool any_active = false;
    for (int i = 0; i < OPENC6_MAX_PROCS; i++) {
        if (s_procs[i].state == PROC_STATE_RUNNING_FG || s_procs[i].state == PROC_STATE_RUNNING_BG) {
            any_active = true;
            break;
        }
    }
    if (!any_active) {
        led_mgmt_apply_aura_from_nvram();
    }

    xSemaphoreGive(s_proc_mux);
    return ESP_OK;
}

/**
 * @brief Forcibly terminates all active background and suspended processes.
 * Invoked during OS shutdown (poweroff/reboot) to cleanly tear down sockets
 * and worker threads before releasing the sandbox arena and hardware PMP slots.
 */
void proc_kill_all(void)
{
    xSemaphoreTake(s_proc_mux, portMAX_DELAY);
    for (int i = 0; i < OPENC6_MAX_PROCS; i++) {
        if (s_procs[i].state != PROC_STATE_FREE && s_procs[i].pid != 0) {
            uint16_t pid = s_procs[i].pid;
            xSemaphoreGive(s_proc_mux);
            proc_kill(pid);
            xSemaphoreTake(s_proc_mux, portMAX_DELAY);
        }
    }
    xSemaphoreGive(s_proc_mux);
}

esp_err_t proc_suspend(uint16_t pid)
{
    xSemaphoreTake(s_proc_mux, portMAX_DELAY);
    openc6_proc_t *proc = NULL;
    for (int i = 0; i < OPENC6_MAX_PROCS; i++) {
        if (s_procs[i].pid == pid &&
            (s_procs[i].state == PROC_STATE_RUNNING_FG || s_procs[i].state == PROC_STATE_RUNNING_BG)) {
            proc = &s_procs[i];
        break;
            }
    }

    if (!proc || !proc->task_handle) {
        xSemaphoreGive(s_proc_mux);
        return ESP_ERR_NOT_FOUND;
    }

    /* Freeze execution thread directly via FreeRTOS scheduler */
    vTaskSuspend(proc->task_handle);
    proc->state = PROC_STATE_SUSPENDED;

    /* If suspended process held foreground console, release it */
    if (s_fg_pid == pid) {
        s_fg_pid = 0;
        if (s_fg_done_sem) {
            xSemaphoreGive(s_fg_done_sem);
        }
    }

    /* ─── ZSWAP: Compress Process Memory Pages in RAM ─────────────────── */
    zc6_swap_mode_t swap_mode = VAL_ZC6_SWAP_DEFAULT;
    nvram_get_zc6_swap_mode(&swap_mode);
    bool swapped_ok = false;
    size_t old_mem_size = proc->mem_size;

    /* Minimum safe heap floor: reserve 30 KB for Wi-Fi buffers and TCP sockets */
    #define ZSWAP_SAFETY_HEAP_WATERMARK (30UL * 1024UL)
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if ((swap_mode == ZC6_SWAP_RAM || swap_mode == ZC6_SWAP_BOTH) &&
        proc->mem_base != 0 && proc->mem_size > 0 && proc->swapped_data == NULL &&
        free_heap >= ZSWAP_SAFETY_HEAP_WATERMARK) {

        size_t max_comp = sizeof(zc6_header_t) + proc->mem_size + 128;
    uint8_t *comp_dst = (uint8_t *)malloc(max_comp);

    if (comp_dst) {
        zc6_mem_buf_t out_buf = { .data = comp_dst, .size = 0, .capacity = max_comp };
        if (zc6_compress((const uint8_t *)proc->mem_base, proc->mem_size, &out_buf) &&
            out_buf.size < proc->mem_size) {

            /* Shrink swap buffer to exact compressed byte size */
            uint8_t *shrunk = (uint8_t *)realloc(comp_dst, out_buf.size);
        proc->swapped_data = shrunk ? shrunk : comp_dst;
        proc->swapped_size = out_buf.size;

        /* Reclaim arena pages back to dynamic pool! */
        proc_mempool_free((void *)proc->mem_base, proc->mem_size);
        proc->state = PROC_STATE_SWAPPED;
        swapped_ok = true;
            } else {
                free(comp_dst);
            }
    }
        }

        /* Restore Aura preferences only if no other background payload is running */
        bool any_active = false;
        for (int i = 0; i < OPENC6_MAX_PROCS; i++) {
            if (s_procs[i].state == PROC_STATE_RUNNING_BG) {
                any_active = true;
                break;
            }
        }
        if (!any_active) {
            led_mgmt_apply_aura_from_nvram();
        }

        xSemaphoreGive(s_proc_mux);

        char msg[128];
        if (swapped_ok) {
            snprintf(msg, sizeof(msg), "\r\n[ZSWAP %u]+ Process '%s' swapped to RAM (%zu KB -> %zu B, Saved %zu KB)\r\n",
                     pid, proc->name, old_mem_size / 1024, proc->swapped_size, (old_mem_size - proc->swapped_size) / 1024);
        } else {
            snprintf(msg, sizeof(msg), "\r\n[JOB %u]+ Suspended '%s' (Use 'fg' or 'bg' to resume)\r\n",
                     pid, proc->name);
        }
        hw_usb_print(msg);
        return ESP_OK;
}

esp_err_t proc_suspend_fg(void)
{
    if (s_fg_pid == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    return proc_suspend(s_fg_pid);
}

/**
 * @brief Restores process memory pages from ZSWAP back into arena.
 */
static esp_err_t proc_unswap_if_needed(openc6_proc_t *proc)
{
    if (proc->state != PROC_STATE_SWAPPED) {
        return ESP_OK;
    }

    /* Reallocate the EXACT physical pages at original base to maintain stack integrity */
    uint8_t *slot_base = (uint8_t *)proc_mempool_alloc_at(proc->mem_base, proc->mem_size);
    if (!slot_base) {
        char err_msg[128];
        snprintf(err_msg, sizeof(err_msg),
                 "Error: ZSWAP collision! Base pages for '%s' are occupied. Suspend active jobs first.\r\n",
                 proc->name);
        hw_usb_print(err_msg);
        return ESP_ERR_INVALID_STATE;
    }

    size_t decomp_len = 0;
    bool ok = zc6_decompress(proc->swapped_data, proc->swapped_size, slot_base, proc->mem_size, &decomp_len);
    free(proc->swapped_data);
    proc->swapped_data = NULL;
    proc->swapped_size = 0;

    if (!ok || decomp_len != proc->mem_size) {
        proc_mempool_free(slot_base, proc->mem_size);
        proc->state = PROC_STATE_FREE;
        hw_usb_print("Error: ZSWAP memory decompression corruption.\r\n");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t proc_resume_bg(uint16_t pid)
{
    xSemaphoreTake(s_proc_mux, portMAX_DELAY);
    openc6_proc_t *proc = NULL;
    for (int i = 0; i < OPENC6_MAX_PROCS; i++) {
        if (s_procs[i].pid == pid &&
            (s_procs[i].state == PROC_STATE_SUSPENDED || s_procs[i].state == PROC_STATE_SWAPPED)) {
            proc = &s_procs[i];
        break;
            }
    }

    if (!proc || !proc->task_handle) {
        xSemaphoreGive(s_proc_mux);
        return ESP_ERR_NOT_FOUND;
    }

    if (proc_unswap_if_needed(proc) != ESP_OK) {
        xSemaphoreGive(s_proc_mux);
        return ESP_ERR_NO_MEM;
    }

    led_mgmt_set_aura_mode(AURA_DISABLED);
    led_mgmt_set_color(0, 0, 0);

    proc->state = PROC_STATE_RUNNING_BG;
    vTaskResume(proc->task_handle);
    xSemaphoreGive(s_proc_mux);

    char msg[80];
    snprintf(msg, sizeof(msg), "[JOB %u] Resumed in background: '%s'\r\n", proc->pid, proc->name);
    hw_usb_print(msg);
    return ESP_OK;
}

esp_err_t proc_resume_fg(uint16_t pid)
{
    xSemaphoreTake(s_proc_mux, portMAX_DELAY);
    openc6_proc_t *proc = NULL;
    for (int i = 0; i < OPENC6_MAX_PROCS; i++) {
        if (s_procs[i].pid == pid &&
            (s_procs[i].state == PROC_STATE_SUSPENDED || s_procs[i].state == PROC_STATE_SWAPPED || s_procs[i].state == PROC_STATE_RUNNING_BG)) {
            proc = &s_procs[i];
        break;
            }
    }

    if (!proc || !proc->task_handle) {
        xSemaphoreGive(s_proc_mux);
        return ESP_ERR_NOT_FOUND;
    }

    if (proc_unswap_if_needed(proc) != ESP_OK) {
        xSemaphoreGive(s_proc_mux);
        return ESP_ERR_NO_MEM;
    }

    led_mgmt_set_aura_mode(AURA_DISABLED);
    led_mgmt_set_color(0, 0, 0);

    proc->state = PROC_STATE_RUNNING_FG;
    s_fg_pid = proc->pid;
    vTaskResume(proc->task_handle);
    xSemaphoreGive(s_proc_mux);

    char msg[96];
    snprintf(msg, sizeof(msg), "[JOB %u] Brought to foreground: '%s'. Press Ctrl+X to suspend.\r\n",
             proc->pid, proc->name);
    hw_usb_print(msg);

    proc_wait_fg_loop();
    return ESP_OK;
}

void proc_print_top(void)
{
    hw_usb_print("\r\n===================================================================================\r\n");
    hw_usb_print(" PID  | COMMAND            | STATE       | ALLOC (KB) | BIN (B)    | TIME (s)\r\n");
    hw_usb_print("===================================================================================\r\n");

    /* Print kernel shell reference entry */
    uint32_t uptime_sec = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    char row_kernel[128];
    snprintf(row_kernel, sizeof(row_kernel), " %-4u | %-18s | %-11s | %-10s | %-10s | %lu\r\n",
             0, "kernel/shell", s_fg_pid == 0 ? "FOREGROUND" : "BACKGROUND", "KERNEL", "-", (unsigned long)uptime_sec);
    hw_usb_print(row_kernel);

    xSemaphoreTake(s_proc_mux, portMAX_DELAY);
    int active_jobs = 0;
    for (int i = 0; i < OPENC6_MAX_PROCS; i++) {
        if (s_procs[i].state != PROC_STATE_FREE) {
            active_jobs++;
            const char *state_str = "UNKNOWN";
            switch (s_procs[i].state) {
                case PROC_STATE_RUNNING_FG: state_str = "RUNNING_FG"; break;
                case PROC_STATE_RUNNING_BG: state_str = "RUNNING_BG"; break;
                case PROC_STATE_SUSPENDED:  state_str = "SUSPENDED";  break;
                case PROC_STATE_SWAPPED:    state_str = "SWAPPED";     break;
                case PROC_STATE_ZOMBIE:     state_str = "ZOMBIE";     break;
                default: break;
            }

            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
            uint32_t runtime_sec = (now_ms >= s_procs[i].start_time_ms) ?
            (now_ms - s_procs[i].start_time_ms) / 1000U : 0U;

            char row[128];
            snprintf(row, sizeof(row), " %-4u | %-18.18s | %-11s | %-10lu | %-10lu | %lu\r\n",
                     s_procs[i].pid,
                     s_procs[i].name,
                     state_str,
                     (unsigned long)(s_procs[i].mem_size / 1024),
                     (unsigned long)s_procs[i].bin_size,
                     (unsigned long)runtime_sec);
            hw_usb_print(row);
        }
    }
    xSemaphoreGive(s_proc_mux);

    /* Render memory pool telemetry breakdown */
    proc_mem_stats_t mem_st;
    proc_get_mem_stats(&mem_st);

    char footer[320];
    snprintf(footer, sizeof(footer),
             "===================================================================================\r\n"
             " Total Jobs: %d | Page Pool: %zu/%zu KB Free (Largest Contig: %zu KB)\r\n"
             " CPU Governor: %lu%% (%lu MHz) | System Free DRAM: %zu KB\r\n\r\n",
             active_jobs,
             mem_st.free_bytes / 1024,
             mem_st.total_bytes / 1024,
             mem_st.largest_free_block / 1024,
             (unsigned long)ulp_me_cpu_load,
             (unsigned long)ulp_me_target_freq,
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024);
    hw_usb_print(footer);
}

uint16_t proc_get_fg_pid(void)
{
    return s_fg_pid;
}

bool proc_is_fg_active(void)
{
    return s_fg_pid != 0;
}
