#include "sandbox.h"
#include "openc6_syscall.h"
#include "openc6_abi.h"
#include "hw_usb.h"
#include "management_engine.h"
#include "hal_gpio.h"
#include "hal_pmp.h"
#include "hal_cpu.h"
#include "hal_wdt.h"
#include "hal_rng.h"
#include "hal_timer.h"
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "led_mgmt.h"
#include "lwip/sockets.h"
#include "esp_netif.h"
#include "hal_flash.h"
#include "openc6_fs.h"
#include <math.h>

#define SANDBOX_BLOCK_MAGIC      0x53424D4FUL

/* Trap Telemetry: 1 - verbose trap logging to USB, 0 - clean/silent production mode */
#define SANDBOX_DEBUG_TRAPS      0

#define MAX_SANDBOX_TASKS 8U

#define SANDBOX_MAX_SOCKETS 8U

static const char *TAG = "SANDBOX";

extern uint8_t *g_sandbox_arena;
extern size_t g_sandbox_arena_size;

extern void sandbox_vector_table(void);
extern void sandbox_trap_vector(void);

typedef struct sandbox_mem_hdr {
    size_t size;
    struct sandbox_mem_hdr *next;
    uint32_t magic;
} sandbox_mem_hdr_t;

/* Isolated state block per concurrent process */
typedef struct {
    TaskHandle_t       task;
    jmp_buf           *jmp;
    uintptr_t          heap_start;
    uintptr_t          heap_break;
    uintptr_t          heap_limit;
    sandbox_mem_hdr_t *free_list;
    sandbox_status_t   exit_status;
    int                sockets[SANDBOX_MAX_SOCKETS];
    uint8_t            trap_stack[SANDBOX_STACK_SIZE] __attribute__((aligned(16)));
} sandbox_task_state_t;

static sandbox_task_state_t s_task_states[MAX_SANDBOX_TASKS];
static int s_active_sandboxes = 0;
static portMUX_TYPE s_sandbox_lock = portMUX_INITIALIZER_UNLOCKED;

/**
 * @brief Registers a newly spawned FreeRTOS task into the sandbox process table.
 *
 * @param task FreeRTOS task handle.
 * @param jmp Non-local recovery jump buffer.
 * @param h_start Physical starting boundary of process heap.
 * @param h_limit Upper physical boundary of process heap.
 * @return Pointer to allocated state slot, or NULL if table is exhausted.
 */
static sandbox_task_state_t* sandbox_register_task(TaskHandle_t task, jmp_buf *jmp,
                                                   uintptr_t h_start, uintptr_t h_limit)
{
    portENTER_CRITICAL(&s_sandbox_lock);
    for (int i = 0; i < MAX_SANDBOX_TASKS; i++) {
        if (s_task_states[i].task == NULL || s_task_states[i].task == task) {
            s_task_states[i].task = task;
            s_task_states[i].jmp = jmp;
            s_task_states[i].heap_start = h_start;
            s_task_states[i].heap_break = h_start;
            s_task_states[i].heap_limit = h_limit;
            s_task_states[i].free_list = NULL;
            s_task_states[i].exit_status = SANDBOX_STATUS_OK;

            for (int s = 0; s < SANDBOX_MAX_SOCKETS; s++) {
                s_task_states[i].sockets[s] = -1;
            }

            portEXIT_CRITICAL(&s_sandbox_lock);
            return &s_task_states[i];
        }
    }
    portEXIT_CRITICAL(&s_sandbox_lock);
    return NULL;
}

static void sandbox_unregister_task(TaskHandle_t task)
{
    portENTER_CRITICAL(&s_sandbox_lock);
    for (int i = 0; i < MAX_SANDBOX_TASKS; i++) {
        if (s_task_states[i].task == task) {
            s_task_states[i].task = NULL;
            s_task_states[i].jmp = NULL;
            s_task_states[i].heap_break = 0;
            s_task_states[i].heap_limit = 0;
            s_task_states[i].free_list = NULL;
            portEXIT_CRITICAL(&s_sandbox_lock);
            return;
        }
    }
    portEXIT_CRITICAL(&s_sandbox_lock);
}

static sandbox_task_state_t* sandbox_get_current_task_state(void)
{
    TaskHandle_t current = xTaskGetCurrentTaskHandle();
    for (int i = 0; i < MAX_SANDBOX_TASKS; i++) {
        if (s_task_states[i].task == current) {
            return &s_task_states[i];
        }
    }
    return NULL;
}

static jmp_buf g_sandbox_exit_jmp;
static sandbox_status_t g_exit_status = SANDBOX_STATUS_OK;

/* Host FreeRTOS CSR context preservation */
static uint32_t g_saved_mtvec = 0;
static uint32_t g_saved_mie = 0;
static uint32_t g_saved_mscratch = 0;
static bool s_sandbox_active = false;

static const openc6_abi_t *g_abi = NULL;
static uintptr_t g_user_heap_break = 0;
static sandbox_mem_hdr_t *s_free_list = NULL;

static void trap_print_hex(uint32_t val)
{
    char buf[11];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 7; i >= 0; i--) {
        uint8_t nibble = (val >> (i * 4)) & 0x0F;
        buf[9 - i] = (nibble < 10) ? ('0' + nibble) : ('A' + nibble - 10);
    }
    buf[10] = '\0';
    hw_usb_print(buf);
}

/**
 * @brief Shuts down and closes all network sockets owned by a specific process slot.
 * Closes allocated file descriptors directly via lwIP close() without invoking shutdown()
 * on passive listening sockets to prevent kernel asserts and deadlocks.
 *
 * @param task FreeRTOS task handle whose descriptors must be closed.
 */
void sandbox_close_task_sockets(TaskHandle_t task)
{
    if (task == NULL) {
        return;
    }

    portENTER_CRITICAL(&s_sandbox_lock);
    for (int i = 0; i < MAX_SANDBOX_TASKS; i++) {
        if (s_task_states[i].task == task) {
            for (int s = 0; s < SANDBOX_MAX_SOCKETS; s++) {
                int fd = s_task_states[i].sockets[s];
                if (fd >= 0) {
                    s_task_states[i].sockets[s] = -1;
                    portEXIT_CRITICAL(&s_sandbox_lock);

                    /* Direct close bypasses invalid TCP shutdown state on LISTEN sockets */
                    close(fd);

                    portENTER_CRITICAL(&s_sandbox_lock);
                }
            }
            break;
        }
    }
    portEXIT_CRITICAL(&s_sandbox_lock);
}

/**
 * @brief Cleans up process table metadata and decrements active sandbox reference count.
 * Must be invoked ONLY AFTER the targeted task has been terminated via vTaskDelete()
 * to prevent executing unprivileged U-mode instructions with restored host MTVEC/MSCRATCH.
 *
 * @param task Terminated FreeRTOS task handle.
 */
void sandbox_unregister_task_external(TaskHandle_t task)
{
    if (task == NULL) {
        return;
    }

    portENTER_CRITICAL(&s_sandbox_lock);
    for (int i = 0; i < MAX_SANDBOX_TASKS; i++) {
        if (s_task_states[i].task == task) {
            s_task_states[i].task = NULL;
            s_task_states[i].jmp = NULL;
            s_task_states[i].heap_break = 0;
            s_task_states[i].heap_limit = 0;
            s_task_states[i].free_list = NULL;

            s_active_sandboxes--;
            if (s_active_sandboxes <= 0) {
                s_active_sandboxes = 0;

                /* Restore host FreeRTOS hardware execution environment */
                hal_cpu_write_mscratch(g_saved_mscratch);
                hal_cpu_write_mtvec(g_saved_mtvec);
                hal_cpu_write_mie(g_saved_mie);
                hal_cpu_restore_gp();
                hal_cpu_force_mmode_mpp();
                hal_wdt_timg_restore();
                s_sandbox_active = false;
            }
            break;
        }
    }
    portEXIT_CRITICAL(&s_sandbox_lock);
}

void* sandbox_alloc(uint32_t size)
{
    if (size == 0 || !g_sandbox_arena) {
        return NULL;
    }

    size_t aligned_size = (size + 15UL) & ~15UL;
    size_t total_size = aligned_size + sizeof(sandbox_mem_hdr_t);

    sandbox_task_state_t *st = sandbox_get_current_task_state();
    if (st != NULL && st->heap_break != 0) {
        /* Allocate from task's isolated slot heap */
        sandbox_mem_hdr_t **curr = &st->free_list;
        while (*curr != NULL) {
            if ((*curr)->size >= aligned_size) {
                sandbox_mem_hdr_t *reused = *curr;
                *curr = reused->next;
                reused->magic = SANDBOX_BLOCK_MAGIC;
                return (void *)((uintptr_t)reused + sizeof(sandbox_mem_hdr_t));
            }
            curr = &(*curr)->next;
        }

        if ((st->heap_break + total_size) > st->heap_limit) {
            return NULL;
        }

        sandbox_mem_hdr_t *block = (sandbox_mem_hdr_t *)st->heap_break;
        block->size = aligned_size;
        block->magic = SANDBOX_BLOCK_MAGIC;
        block->next = NULL;

        st->heap_break += total_size;
        return (void *)((uintptr_t)block + sizeof(sandbox_mem_hdr_t));
    }

    /* Fallback global arena allocator for single-task runs */
    uintptr_t arena_limit = (uintptr_t)g_sandbox_arena + g_sandbox_arena_size - (8UL * 1024UL);
    if ((g_user_heap_break + total_size) > arena_limit) {
        return NULL;
    }

    sandbox_mem_hdr_t *block = (sandbox_mem_hdr_t *)g_user_heap_break;
    block->size = aligned_size;
    block->magic = SANDBOX_BLOCK_MAGIC;
    block->next = NULL;

    g_user_heap_break += total_size;
    return (void *)((uintptr_t)block + sizeof(sandbox_mem_hdr_t));
}

void sandbox_free_mem(void *ptr)
{
    if (!ptr || !g_sandbox_arena) {
        return;
    }

    uintptr_t addr = (uintptr_t)ptr;
    sandbox_mem_hdr_t *block = (sandbox_mem_hdr_t *)(addr - sizeof(sandbox_mem_hdr_t));
    if (block->magic != SANDBOX_BLOCK_MAGIC) {
        return;
    }
    block->magic = 0;

    sandbox_task_state_t *st = sandbox_get_current_task_state();
    if (st != NULL && st->heap_break != 0) {
        block->next = st->free_list;
        st->free_list = block;
    } else {
        block->next = s_free_list;
        s_free_list = block;
    }
}

bool sandbox_is_running(void)
{
    return s_sandbox_active;
}

void sandbox_request_exit(sandbox_status_t status)
{
    sandbox_task_state_t *st = sandbox_get_current_task_state();
    if (st && st->jmp) {
        st->exit_status = status;
        longjmp(*st->jmp, 1);
    }
    if (s_sandbox_active) {
        g_exit_status = status;
        longjmp(g_sandbox_exit_jmp, 1);
    }
    abort();
}

bool sandbox_init_dynamic(const openc6_abi_t *abi, size_t kernel_reserve_bytes)
{
    if (abi != NULL) {
        g_abi = abi;
    }

    if (g_sandbox_arena != NULL) {
        return true;
    }

    size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (largest_block <= kernel_reserve_bytes) {
        ESP_LOGE(TAG, "Insufficient DRAM for sandbox: free=%zu, reserve=%zu",
                 largest_block, kernel_reserve_bytes);
        return false;
    }

    size_t alloc_size = (largest_block - kernel_reserve_bytes) & ~15UL;
    g_sandbox_arena = (uint8_t *)heap_caps_malloc(alloc_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!g_sandbox_arena) {
        ESP_LOGE(TAG, "Failed to allocate %zu bytes for sandbox arena", alloc_size);
        return false;
    }

    g_sandbox_arena_size = alloc_size;
    g_user_heap_break = (uintptr_t)g_sandbox_arena;
    s_free_list = NULL;

    uintptr_t start = (uintptr_t)g_sandbox_arena;
    uintptr_t end   = start + g_sandbox_arena_size;
    hal_pmp_setup(start, end);

    ESP_LOGI(TAG, "Dynamic Sandbox ready: %zu KB [0x%08lx - 0x%08lx] (Reserve: %zu KB)",
             alloc_size / 1024, (unsigned long)start, (unsigned long)end, kernel_reserve_bytes / 1024);
    return true;
}

void sandbox_deinit(void)
{
    if (g_sandbox_arena) {
        hal_pmp_clear();
        free(g_sandbox_arena);
        g_sandbox_arena = NULL;
        g_sandbox_arena_size = 0;
        g_user_heap_break = 0;
        s_free_list = NULL;
        s_sandbox_active = false;
        ESP_LOGI(TAG, "Sandbox arena released to heap");
    }
}

uint8_t* sandbox_get_arena(void)
{
    return g_sandbox_arena;
}

size_t sandbox_get_arena_size(void)
{
    return g_sandbox_arena_size;
}

const openc6_abi_t *sandbox_get_abi(void)
{
    return g_abi;
}

bool sandbox_load_payload(const uint8_t *src, size_t len)
{
    if (!src || len == 0 || !g_sandbox_arena || len > g_sandbox_arena_size) {
        return false;
    }

    if (src != g_sandbox_arena) {
        memset(g_sandbox_arena, 0, g_sandbox_arena_size);
        memcpy(g_sandbox_arena, src, len);
    }

    g_user_heap_break = (uintptr_t)g_sandbox_arena + ((len + 15UL) & ~15UL);
    s_free_list = NULL;
    return true;
}

/**
 * @brief Renders a comprehensive, developer-friendly crash dump for U-mode exceptions.
 */
static void sandbox_print_exception_dump(sandbox_trap_frame_t *frame, uint32_t code, uint32_t mtval)
{
    const char *cause_str = "Unknown Exception";
    const char *hint_str  = "Check system register state and payload binary integrity.";

    switch (code) {
        case 0:
            cause_str = "Instruction Address Misaligned (Code: 0)";
            hint_str  = "Branch/jump targeted an unaligned instruction address.";
            break;
        case 1:
            cause_str = "Instruction Access Fault [PMP Execution Denied] (Code: 1)";
            hint_str  = "CPU attempted to execute forbidden memory. Check function pointers or corrupted stack.";
            break;
        case 2:
            cause_str = "Illegal Instruction (Code: 2)";
            hint_str  = "Invalid machine opcode. Verify compiler architecture flags (-march=rv32imac).";
            break;
        case 3:
            cause_str = "Breakpoint Trap (Code: 3)";
            hint_str  = "Software ebreak instruction triggered.";
            break;
        case 4:
            cause_str = "Load Address Misaligned (Code: 4)";
            hint_str  = "Memory read was not naturally aligned for target data type.";
            break;
        case 5:
            cause_str = "Load Access Fault [PMP Read Denied] (Code: 5)";
            hint_str  = "Process tried to read outside its arena. Check NULL pointers (0x0) or .rodata placement.";
            break;
        case 6:
            cause_str = "Store Address Misaligned (Code: 6)";
            hint_str  = "Memory write was not naturally aligned for target data type.";
            break;
        case 7:
            cause_str = "Store Access Fault [PMP Write Denied] (Code: 7)";
            hint_str  = "Process tried to write into protected memory (Kernel/Flash/NULL pointer).";
            break;
        default:
            break;
    }

    hw_usb_print("\r\n======================================================================\r\n");
    hw_usb_print("[KERNEL_PANIC] Unprivileged U-Mode Process Exception Caught!\r\n");
    hw_usb_print("======================================================================\r\n");

    hw_usb_print("  Fault Reason : ");
    hw_usb_print(cause_str);
    hw_usb_print("\r\n");

    hw_usb_print("  Faulting PC  : ");
    trap_print_hex(frame->mepc);
    hw_usb_print("\r\n");

    hw_usb_print("  Return (RA)  : ");
    trap_print_hex(frame->ra);
    hw_usb_print("\r\n");

    hw_usb_print("  Target (TVAL): ");
    trap_print_hex(mtval);
    hw_usb_print("\r\n");

    hw_usb_print("  Syscall (a7) : ");
    trap_print_hex(frame->a7);
    hw_usb_print(" | Last Arg (a0): ");
    trap_print_hex(frame->a0);
    hw_usb_print("\r\n");

    hw_usb_print("----------------------------------------------------------------------\r\n");
    hw_usb_print("  Debug Hint   : ");
    hw_usb_print(hint_str);
    hw_usb_print("\r\n======================================================================\r\n\r\n");
}

static bool sandbox_is_valid_read_addr(uintptr_t addr, size_t len)
{
    uintptr_t a_start = (uintptr_t)g_sandbox_arena;
    uintptr_t a_end   = a_start + g_sandbox_arena_size;
    bool in_arena = (addr >= a_start && addr <= a_end && len <= (a_end - addr));
    bool in_flash = hal_cpu_is_rodata_range(addr, len);
    return in_arena || in_flash;
}

static bool sandbox_is_valid_write_addr(uintptr_t addr, size_t len)
{
    uintptr_t a_start = (uintptr_t)g_sandbox_arena;
    uintptr_t a_end   = a_start + g_sandbox_arena_size;
    return (addr >= a_start && addr <= a_end && len <= (a_end - addr));
}

static bool sandbox_copy_user_str(uintptr_t user_ptr, char *dst, size_t max_len)
{
    if (user_ptr == 0 || max_len == 0) {
        return false;
    }
    for (size_t i = 0; i < max_len; i++) {
        if (!sandbox_is_valid_read_addr(user_ptr + i, 1)) {
            return false;
        }
        char c = ((const char *)user_ptr)[i];
        dst[i] = c;
        if (c == '\0') {
            return i > 0;
        }
    }
    dst[max_len - 1] = '\0';
    return true;
}

int sandbox_dispatch_trap(sandbox_trap_frame_t *frame, uint32_t mcause, uint32_t mtval)
{
    bool is_interrupt = (mcause & (1UL << 31)) != 0;
    uint32_t code = mcause & ~(1UL << 31);

    /* In Vectored Mode, interrupts never route here. If an errant interrupt hits, keep mie alive */
    if (is_interrupt) {
        return 0;
    }

    /* Force Machine Mode (MPP = 3) during C trap handler execution so ESP-IDF spinlocks
     * never attempt to touch the non-existent 'ustatus' CSR on ESP32-C6 silicon! */
    hal_cpu_force_mmode_mpp();

    #if SANDBOX_DEBUG_TRAPS
    hw_usb_print("[TRAP_DISPATCH] Trap entered! Cause=");
    trap_print_hex(code);
    hw_usb_print(" PC=");
    trap_print_hex(frame->mepc);
    hw_usb_print(" RA=");
    trap_print_hex(frame->ra);
    hw_usb_print(" a7=");
    trap_print_hex(frame->a7);
    hw_usb_print(" mtval=");
    trap_print_hex(mtval);
    hw_usb_print("\r\n");
    #endif

    /* ECALL trap from U-mode (mcause 8) or M-mode (mcause 11) */
    if (code == 8 || code == 11) {
        frame->mepc += 4;
        uint32_t sys_id = frame->a7;

        /* Enable interrupts during syscall execution so lwIP tcpip_thread can service sockets */
        hal_cpu_irq_enable();

        switch (sys_id) {
            case SYS_EXIT: {
                sandbox_task_state_t *st = sandbox_get_current_task_state();
                if (st && st->jmp) {
                    st->exit_status = (sandbox_status_t)frame->a0;
                    longjmp(*st->jmp, 1);
                } else {
                    g_exit_status = (sandbox_status_t)frame->a0;
                    longjmp(g_sandbox_exit_jmp, 1);
                }
                break;
            }

            case SYS_PRINT: {
                uintptr_t str_addr = (uintptr_t)frame->a0;
                uintptr_t a_start = (uintptr_t)g_sandbox_arena;
                uintptr_t a_end   = a_start + g_sandbox_arena_size;
                if ((str_addr >= a_start && str_addr < a_end) ||
                    hal_cpu_is_rodata_addr(str_addr)) {
                    hw_usb_print((const char *)str_addr);
                    }
                    frame->a0 = 0;
                break;
            }

            case SYS_DELAY_MS: {
                management_engine_pet_watchdog();
                uint32_t ms = (uint32_t)frame->a0;
                if (ms >= portTICK_PERIOD_MS) {
                    vTaskDelay(pdMS_TO_TICKS(ms));
                } else if (ms > 0) {
                    while (ms--) {
                        hal_timer_delay_us(1000);
                    }
                }
                frame->a0 = 0;
                break;
            }

            case SYS_SYS_RESET: {
                sandbox_task_state_t *st = sandbox_get_current_task_state();
                if (st && st->jmp) {
                    st->exit_status = SANDBOX_STATUS_OK;
                    longjmp(*st->jmp, 1);
                }
                g_exit_status = SANDBOX_STATUS_OK;
                longjmp(g_sandbox_exit_jmp, 1);
                break;
            }

            case SYS_SET_LED_COLOR: {
                led_mgmt_set_color((uint8_t)frame->a0, (uint8_t)frame->a1, (uint8_t)frame->a2);
                frame->a0 = 0;
                break;
            }

            case SYS_GET_RANDOM:
                frame->a0 = hal_rng_get_u32();
                break;

            case SYS_GET_FREE_RAM:
                frame->a0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                break;

            case SYS_GET_TOTAL_RAM:
                frame->a0 = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                break;

            case SYS_GET_TOTAL_FLASH:
                frame->a0 = hal_flash_get_chip_size();
                break;

            case SYS_MALLOC:
                frame->a0 = (uint32_t)sandbox_alloc((uint32_t)frame->a0);
                break;

            case SYS_FREE:
                sandbox_free_mem((void *)frame->a0);
                frame->a0 = 0;
                break;

            case SYS_SHA256: {
                extern void bios_sha256_compute(const uint8_t *input, uint32_t len, uint8_t *output);
                const uint8_t *input = (const uint8_t *)frame->a0;
                uint32_t len = frame->a1;
                uint8_t *output = (uint8_t *)frame->a2;
                uintptr_t a_start = (uintptr_t)g_sandbox_arena;
                uintptr_t a_end   = a_start + g_sandbox_arena_size;
                if ((uintptr_t)output >= a_start && ((uintptr_t)output + 32) <= a_end) {
                    bios_sha256_compute(input, len, output);
                    frame->a0 = 0;
                } else {
                    frame->a0 = (uint32_t)-1;
                }
                break;
            }

            case SYS_MATH_ISQRT: {
                uint32_t x = frame->a0;
                uint32_t res = 0;
                uint32_t bit = 1UL << 30;
                while (bit > x) bit >>= 2;
                while (bit != 0) {
                    if (x >= res + bit) {
                        x -= res + bit;
                        res = (res >> 1) + bit;
                    } else {
                        res >>= 1;
                    }
                    bit >>= 2;
                }
                frame->a0 = res;
                break;
            }

            case SYS_MATH_SIN_DEG: {
                int32_t deg = (int32_t)frame->a0;
                double rad = deg * (3.14159265359 / 180.0);
                frame->a0 = (uint32_t)(int32_t)(sin(rad) * 10000.0);
                break;
            }

            case SYS_MATH_COS_DEG: {
                int32_t deg = (int32_t)frame->a0;
                double rad = deg * (3.14159265359 / 180.0);
                frame->a0 = (uint32_t)(int32_t)(cos(rad) * 10000.0);
                break;
            }

            case SYS_WIFI_IS_CONNECTED: {
                extern bool wifi_mgmt_is_connected(void);
                frame->a0 = wifi_mgmt_is_connected() ? 1 : 0;
                break;
            }

            case SYS_NET_GET_IP: {
                char *buf = (char *)frame->a0;
                uint32_t max_len = frame->a1;
                uintptr_t a_start = (uintptr_t)g_sandbox_arena;
                uintptr_t a_end   = a_start + g_sandbox_arena_size;

                if ((uintptr_t)buf >= a_start && ((uintptr_t)buf + max_len) <= a_end) {
                    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                    esp_netif_ip_info_t ip_info;
                    if (sta && esp_netif_get_ip_info(sta, &ip_info) == ESP_OK) {
                        snprintf(buf, max_len, IPSTR, IP2STR(&ip_info.ip));
                        frame->a0 = 0;
                    } else {
                        frame->a0 = (uint32_t)-1;
                    }
                } else {
                    frame->a0 = (uint32_t)-1;
                }
                break;
            }

            case SYS_TCP_LISTEN: {
                uint16_t port = (uint16_t)frame->a0;
                int s = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
                if (s < 0) {
                    frame->a0 = (uint32_t)-1;
                    break;
                }
                int opt = 1;
                setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

                struct sockaddr_in serv_addr = {
                    .sin_family = AF_INET,
                    .sin_addr.s_addr = htonl(INADDR_ANY),
                    .sin_port = htons(port)
                };
                if (bind(s, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0 || listen(s, 4) < 0) {
                    close(s);
                    frame->a0 = (uint32_t)-1;
                    break;
                }

                /* Track listening socket in process descriptor table for graceful teardown */
                sandbox_task_state_t *st = sandbox_get_current_task_state();
                if (st != NULL) {
                    for (int i = 0; i < SANDBOX_MAX_SOCKETS; i++) {
                        if (st->sockets[i] < 0) {
                            st->sockets[i] = s;
                            break;
                        }
                    }
                }

                frame->a0 = (uint32_t)s;
                break;
            }

            case SYS_TCP_ACCEPT: {
                management_engine_pet_watchdog();
                int s = (int)frame->a0;
                uint32_t timeout_ms = frame->a1;

                /* Apply receive timeout directly to socket options, bypassing lwIP select_cb_list */
                struct timeval tv = {
                    .tv_sec = timeout_ms / 1000,
                    .tv_usec = (timeout_ms % 1000) * 1000
                };
                setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

                struct sockaddr_in client_addr;
                socklen_t addr_len = sizeof(client_addr);
                int client = accept(s, (struct sockaddr *)&client_addr, &addr_len);

                if (client >= 0) {
                    sandbox_task_state_t *st = sandbox_get_current_task_state();
                    if (st != NULL) {
                        for (int i = 0; i < SANDBOX_MAX_SOCKETS; i++) {
                            if (st->sockets[i] < 0) {
                                st->sockets[i] = client;
                                break;
                            }
                        }
                    }
                    frame->a0 = (uint32_t)client;
                } else {
                    frame->a0 = (uint32_t)-1;
                }
                break;
            }

            case SYS_TCP_READ: {
                int fd = (int)frame->a0;
                uintptr_t buf_addr = (uintptr_t)frame->a1;
                uint32_t len = frame->a2;
                uintptr_t a_start = (uintptr_t)g_sandbox_arena;
                uintptr_t a_end   = a_start + g_sandbox_arena_size;

                if (buf_addr >= a_start && (buf_addr + len) <= a_end) {
                    ssize_t r = recv(fd, (void *)buf_addr, len, 0);
                    frame->a0 = (uint32_t)r;
                } else {
                    frame->a0 = (uint32_t)-1;
                }
                break;
            }

            case SYS_TCP_WRITE: {
                int fd = (int)frame->a0;
                uintptr_t buf_addr = (uintptr_t)frame->a1;
                uint32_t len = frame->a2;
                uintptr_t a_start = (uintptr_t)g_sandbox_arena;
                uintptr_t a_end   = a_start + g_sandbox_arena_size;

                bool in_arena = (buf_addr >= a_start && (buf_addr + len) <= a_end);
                bool in_flash = hal_cpu_is_rodata_range(buf_addr, len);

                if (in_arena || in_flash) {
                    ssize_t w = send(fd, (const void *)buf_addr, len, 0);
                    frame->a0 = (uint32_t)w;
                } else {
                    frame->a0 = (uint32_t)-1;
                }
                break;
            }

            case SYS_TCP_CLOSE: {
                int fd = (int)frame->a0;
                if (fd >= 0) {
                    sandbox_task_state_t *st = sandbox_get_current_task_state();
                    if (st != NULL) {
                        for (int i = 0; i < SANDBOX_MAX_SOCKETS; i++) {
                            if (st->sockets[i] == fd) {
                                st->sockets[i] = -1;
                                break;
                            }
                        }
                    }
                    shutdown(fd, SHUT_WR);
                    hal_timer_delay_us(30000);
                    close(fd);
                }
                frame->a0 = 0;
                break;
            }

            case SYS_SBRK: {
                intptr_t incr = (intptr_t)frame->a0;
                sandbox_task_state_t *st = sandbox_get_current_task_state();
                if (st != NULL && st->heap_break != 0) {
                    uintptr_t prev_break = st->heap_break;
                    if (incr == 0) {
                        frame->a0 = prev_break;
                    } else if (incr > 0 && (prev_break + (uintptr_t)incr) <= st->heap_limit) {
                        st->heap_break += (uintptr_t)incr;
                        frame->a0 = prev_break;
                    } else if (incr < 0 && (prev_break - (uintptr_t)(-incr)) >= st->heap_start) {
                        st->heap_break -= (uintptr_t)(-incr);
                        frame->a0 = prev_break;
                    } else {
                        frame->a0 = (uint32_t)-1;
                    }
                    break;
                }

                uintptr_t arena_limit = (uintptr_t)g_sandbox_arena + g_sandbox_arena_size - (8UL * 1024UL);
                uintptr_t prev_break = g_user_heap_break;

                if (incr == 0) {
                    frame->a0 = prev_break;
                } else if (incr > 0 && (prev_break + (uintptr_t)incr) <= arena_limit) {
                    g_user_heap_break += (uintptr_t)incr;
                    frame->a0 = prev_break;
                } else if (incr < 0 && (prev_break - (uintptr_t)(-incr)) >= (uintptr_t)g_sandbox_arena) {
                    g_user_heap_break -= (uintptr_t)(-incr);
                    frame->a0 = prev_break;
                } else {
                    frame->a0 = (uint32_t)-1;
                }
                break;
            }

            case SYS_FS_WRITE_FILE: {
                uintptr_t name_addr = (uintptr_t)frame->a0;
                uintptr_t data_addr = (uintptr_t)frame->a1;
                uint32_t len        = frame->a2;
                uint16_t parent_id  = (uint16_t)frame->a3;
                (void)frame->a4; /* force */

                char safe_name[18];
                if (!sandbox_copy_user_str(name_addr, safe_name, sizeof(safe_name))) {
                    frame->a0 = (uint32_t)-1;
                    break;
                }
                if (len > 0 && !sandbox_is_valid_read_addr(data_addr, len)) {
                    frame->a0 = (uint32_t)-1;
                    break;
                }

                int16_t id = fs_write_file(safe_name, (const uint8_t *)data_addr, len, parent_id);
                frame->a0 = (id >= 0) ? (uint32_t)id : (uint32_t)-1;
                break;
            }

            case SYS_FS_READ_FILE: {
                uintptr_t name_addr = (uintptr_t)frame->a0;
                uintptr_t dest_addr = (uintptr_t)frame->a1;
                uint32_t offset     = frame->a2;
                uint32_t len        = frame->a3;
                uint16_t parent_id  = (uint16_t)frame->a4;

                char safe_name[18];
                if (!sandbox_copy_user_str(name_addr, safe_name, sizeof(safe_name))) {
                    frame->a0 = (uint32_t)-1;
                    break;
                }
                if (len == 0) {
                    frame->a0 = 0;
                    break;
                }
                if (!sandbox_is_valid_write_addr(dest_addr, len)) {
                    frame->a0 = (uint32_t)-1;
                    break;
                }

                int16_t id = fs_find_id(safe_name, parent_id);
                if (id < 0) {
                    frame->a0 = (uint32_t)-1;
                    break;
                }
                frame->a0 = (uint32_t)fs_read_file((uint16_t)id, (uint8_t *)dest_addr, offset, len);
                break;
            }

            case SYS_FS_DELETE: {
                uintptr_t name_addr = (uintptr_t)frame->a0;
                uint16_t parent_id  = (uint16_t)frame->a1;

                char safe_name[18];
                if (sandbox_copy_user_str(name_addr, safe_name, sizeof(safe_name))) {
                    int16_t id = fs_find_id(safe_name, parent_id);
                    if (id >= 0) {
                        fs_delete((uint16_t)id);
                    }
                }
                frame->a0 = 0;
                break;
            }

            case SYS_GPIO_SET_DIR:
                frame->a0 = (uint32_t)hal_gpio_set_direction((uint32_t)frame->a0, (uint32_t)frame->a1);
                break;

            case SYS_GPIO_WRITE:
                frame->a0 = (uint32_t)hal_gpio_write_pin((uint32_t)frame->a0, (uint32_t)frame->a1);
                break;

            case SYS_GPIO_READ:
                frame->a0 = (uint32_t)hal_gpio_read_pin((uint32_t)frame->a0);
                break;

            default:
                hw_usb_print("[TRAP_WARN] Unknown syscall ID! Returning -1\r\n");
                frame->a0 = (uint32_t)-1;
                break;
        }

        /* Disarm interrupts before returning context to mret */
        hal_cpu_irq_disable_only();

        /* Restore privilege mode: return to U-mode only for U-mode ecall (code 8); preserve M-mode for kernel yields */
        hal_cpu_set_mret_mode(code == 8);
        return 0;
    }

    /* Render complete diagnostic crash dump for unprivileged payload debugging */
    sandbox_print_exception_dump(frame, code, mtval);

    sandbox_task_state_t *st = sandbox_get_current_task_state();
    if (st && st->jmp) {
        st->exit_status = SANDBOX_STATUS_ERR_FAULT;
        longjmp(*st->jmp, 1);
    } else {
        g_exit_status = SANDBOX_STATUS_ERR_FAULT;
        longjmp(g_sandbox_exit_jmp, 1);
    }
    return 0;
}

/**
 * @brief Executes a sandboxed U-mode payload in a designated memory slot.
 * Provisions task-local trap stack and recovery jump contexts to allow concurrent multi-process execution.
 */
sandbox_status_t sandbox_run_slot(uintptr_t entry, uintptr_t user_sp, size_t bin_size)
{
    if (!g_sandbox_arena || g_sandbox_arena_size == 0) {
        hw_usb_print("\r\n[SANDBOX] Error: Arena not allocated!\r\n");
        return SANDBOX_STATUS_ERR_NO_MEM;
    }

    uintptr_t abi_ptr = (uintptr_t)sandbox_get_abi();
    management_engine_pet_watchdog();

    /* Unsubscribe worker task from Task Watchdog */
    esp_task_wdt_delete(NULL);

    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    jmp_buf task_exit_jmp;

    /* Heap starts right after the binary image (16-byte aligned), ends below stack */
    size_t code_span = (bin_size > 0) ? ((bin_size + 15UL) & ~15UL) : (8UL * 1024UL);
    uintptr_t slot_heap_start = entry + code_span;
    uintptr_t slot_heap_limit = user_sp - (4UL * 1024UL);

    sandbox_task_state_t *st = sandbox_register_task(self, &task_exit_jmp, slot_heap_start, slot_heap_limit);
    if (!st) {
        hw_usb_print("\r\n[SANDBOX] Error: Process table full (No task state slots available)!\r\n");
        return SANDBOX_STATUS_ERR_NO_MEM;
    }

    if (setjmp(task_exit_jmp) != 0) {
        hal_wdt_timg_clear_and_halt();
        hal_cpu_clear_mip();

        portENTER_CRITICAL(&s_sandbox_lock);
        s_active_sandboxes--;
        if (s_active_sandboxes <= 0) {
            s_active_sandboxes = 0;
            hal_cpu_write_mscratch(g_saved_mscratch);
            hal_cpu_write_mtvec(g_saved_mtvec);
            hal_cpu_write_mie(g_saved_mie);
            hal_cpu_restore_gp();
            hal_cpu_force_mmode_mpp();
            hal_wdt_timg_restore();
            s_sandbox_active = false;
        }
        portEXIT_CRITICAL(&s_sandbox_lock);

        hal_cpu_irq_enable();
        sandbox_status_t final_status = st->exit_status;
        sandbox_unregister_task(self);
        return final_status;
    }

    portENTER_CRITICAL(&s_sandbox_lock);
    if (s_active_sandboxes == 0) {
        g_saved_mtvec = hal_cpu_swap_mtvec_vectored((uintptr_t)sandbox_vector_table);
        g_saved_mie = hal_cpu_read_mie();
        g_saved_mscratch = hal_cpu_read_mscratch();
        hal_wdt_timg_save_and_disable();
        s_sandbox_active = true;
    }
    s_active_sandboxes++;
    portEXIT_CRITICAL(&s_sandbox_lock);

    /* DEDICATED PER-TASK TRAP STACK */
    uintptr_t kernel_trap_sp = (uintptr_t)&st->trap_stack[SANDBOX_STACK_SIZE];
    hal_cpu_enter_umode(entry, user_sp, abi_ptr, kernel_trap_sp);

    return SANDBOX_STATUS_OK;
}

sandbox_status_t sandbox_run(void)
{
    if (!g_sandbox_arena || g_sandbox_arena_size == 0) {
        hw_usb_print("\r\n[SANDBOX] Error: Arena not allocated!\r\n");
        return SANDBOX_STATUS_ERR_NO_MEM;
    }
    uintptr_t user_sp = (((uintptr_t)g_sandbox_arena + g_sandbox_arena_size) & ~15UL) - 64;
    return sandbox_run_slot((uintptr_t)g_sandbox_arena, user_sp, 0);
}
