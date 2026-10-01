#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "boot_manager.h"
#include "hw_usb.h"
#include "sandbox.h"
#include "openc6_fs.h"
#include "wifi_mgmt.h"
#include "nvram.h"
#include "pxe_boot.h"
#include "management_engine.h"
#include "me_shared.h"
#include "led_mgmt.h"
#include "proc_manager.h"

/* External entry point to transition SoC into S5 Soft-Off deep sleep */
extern void bios_enter_s5_state(void);

/* Standard ASCII control characters for USB serial flow control */
#define CMD_ACK         0x06
#define CMD_EOT         0x04
#define CMD_NAK         0x15

/**
 * @brief Resolves relative or absolute path within the virtual directory structure.
 *
 * @param path Input path string (e.g. "dir/file" or "/file").
 * @param current_dir Current working directory sector ID.
 * @param out_name Destination buffer for extracted target entry name (minimum 18 bytes).
 * @return Parent directory sector ID on success, or -1 if path components are missing.
 */
static int16_t shell_resolve_path(const char *path, uint16_t current_dir, char *out_name)
{
    uint16_t walk_dir = current_dir;
    char temp[128];
    strncpy(temp, path, 127);
    temp[127] = '\0';

    if (temp[0] == '/') {
        walk_dir = 0;
    }

    char *token = strtok(temp, "/");
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
        token = strtok(NULL, "/");
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
 * @brief Streams binary payload from USB host with synchronization preambles and ACKs.
 *
 * @param out_buf Target memory buffer to store incoming stream.
 * @param max_size Maximum allowed capacity of target buffer.
 * @param out_received_size Pointer to store the verified byte count received.
 * @return true on successful stream completion, false on timeout or size mismatch.
 */
static bool shell_serial_receive(uint8_t *out_buf, size_t max_size, uint32_t *out_received_size)
{
    uint8_t size_buf[4] = {0};
    uint32_t file_size = 0;
    uint64_t start_time = esp_timer_get_time();
    uint64_t last_sync_time = 0;
    int state = 0;

    /* Drain stale data from hardware USB OUT FIFO */
    while (hw_usb_has_data()) {
        hw_usb_getc();
    }

    /* Wait for host synchronization preamble (0x5A 0xA5 followed by 4-byte LE size) */
    while ((esp_timer_get_time() - start_time) < 30000000ULL) {
        management_engine_pet_watchdog();
        uint64_t now = esp_timer_get_time();
        if (now - last_sync_time > 500000ULL) {
            hw_usb_print("##OPENC6_SYNC##");
            last_sync_time = now;
        }

        while (hw_usb_has_data()) {
            uint8_t rx_b = (uint8_t)hw_usb_getc();
            if (state == 0) {
                if (rx_b == 0x5A) state = 1;
            } else if (state == 1) {
                if (rx_b == 0xA5) state = 2;
                else state = (rx_b == 0x5A) ? 1 : 0;
            } else if (state >= 2 && state <= 5) {
                size_buf[state - 2] = rx_b;
                state++;
                if (state == 6) break;
            }
        }
        if (state == 6) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (state != 6) {
        hw_usb_print("\r\n[SERIAL] Error: Preamble timeout.\r\n");
        return false;
    }

    file_size = (uint32_t)size_buf[0] |
    ((uint32_t)size_buf[1] << 8) |
    ((uint32_t)size_buf[2] << 16) |
    ((uint32_t)size_buf[3] << 24);

    if (file_size == 0 || file_size > max_size) {
        hw_usb_putc(CMD_NAK);
        hw_usb_flush();
        hw_usb_print("\r\n[SERIAL] Error: File size exceeds capacity.\r\n");
        return false;
    }

    hw_usb_putc(CMD_ACK);
    hw_usb_flush();

    uint32_t total_received = 0;
    int chunk_accum = 0;
    uint64_t last_data_time = esp_timer_get_time();

    /* Receive data bytes in 64-byte chunks with flow-control ACKs */
    while (total_received < file_size) {
        management_engine_pet_watchdog();
        if (hw_usb_has_data()) {
            out_buf[total_received++] = (uint8_t)hw_usb_getc();
            chunk_accum++;
            last_data_time = esp_timer_get_time();

            if (chunk_accum >= 64 || total_received == file_size) {
                hw_usb_putc(CMD_ACK);
                hw_usb_flush();
                chunk_accum = 0;
            }
        }

        /* Abort if transfer stalls for more than 5 seconds */
        if ((esp_timer_get_time() - last_data_time) > 5000000ULL) {
            hw_usb_print("\r\n[SERIAL] Error: Transfer timeout.\r\n");
            return false;
        }

        if (!hw_usb_has_data()) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    /* Await End-of-Transmission (EOT) marker */
    uint64_t eot_timeout = esp_timer_get_time();
    while ((esp_timer_get_time() - eot_timeout) < 1000000ULL) {
        if (hw_usb_has_data()) {
            if ((uint8_t)hw_usb_getc() == CMD_EOT) break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    *out_received_size = total_received;
    return true;
}

/**
 * @brief Handles file duplication and relocation (cp and mv commands) via openc6_fs.
 */
static void shell_cmd_cp_mv(uint16_t current_dir, bool is_move)
{
    char *src_arg = strtok(NULL, " ");
    char *dst_arg = strtok(NULL, " ");
    if (!src_arg || !dst_arg) {
        hw_usb_print(is_move ? "Usage: mv <src> <dst>\r\n" : "Usage: cp <src> <dst>\r\n");
        return;
    }

    char src_name[18];
    int16_t src_pdir = shell_resolve_path(src_arg, current_dir, src_name);
    if (src_pdir < 0 || strlen(src_name) == 0) {
        hw_usb_print("Error: Invalid source path.\r\n");
        return;
    }

    int16_t src_id = fs_find_id(src_name, (uint16_t)src_pdir);
    if (src_id < 0 || fs_get_type(src_id) != TYPE_FILE) {
        hw_usb_print("Error: Source file not found.\r\n");
        return;
    }

    int32_t src_size = fs_get_size(src_id);
    if (src_size < 0) {
        hw_usb_print("Error: Cannot read source file size.\r\n");
        return;
    }

    char dst_name[18];
    int16_t dst_pdir = shell_resolve_path(dst_arg, current_dir, dst_name);
    if (dst_pdir < 0) {
        hw_usb_print("Error: Invalid destination path.\r\n");
        return;
    }

    uint16_t final_dst_dir = (uint16_t)dst_pdir;
    char final_dst_name[18];

    if (strlen(dst_name) == 0) {
        strncpy(final_dst_name, src_name, sizeof(final_dst_name) - 1);
        final_dst_name[sizeof(final_dst_name) - 1] = '\0';
    } else {
        int16_t check_id = fs_find_id(dst_name, (uint16_t)dst_pdir);
        if (check_id >= 0 && fs_get_type(check_id) == TYPE_DIR) {
            final_dst_dir = (uint16_t)check_id;
            strncpy(final_dst_name, src_name, sizeof(final_dst_name) - 1);
            final_dst_name[sizeof(final_dst_name) - 1] = '\0';
        } else {
            strncpy(final_dst_name, dst_name, sizeof(final_dst_name) - 1);
            final_dst_name[sizeof(final_dst_name) - 1] = '\0';
        }
    }

    uint8_t *buf = NULL;
    bool heap_allocated = false;

    if (src_size == 0) {
        if (fs_write_file(final_dst_name, (const uint8_t *)"", 0, final_dst_dir) >= 0) {
            if (is_move) fs_delete((uint16_t)src_id);
            hw_usb_print(is_move ? "File moved successfully.\r\n" : "File copied successfully.\r\n");
        } else {
            hw_usb_print("Error: Write failed.\r\n");
        }
        return;
    }

    buf = (uint8_t *)malloc((size_t)src_size);
    if (!buf) {
        if (sandbox_get_arena() && (size_t)src_size <= sandbox_get_arena_size()) {
            buf = sandbox_get_arena();
        } else {
            hw_usb_print("Error: Insufficient memory for copy buffer.\r\n");
            return;
        }
    } else {
        heap_allocated = true;
    }

    if (fs_read_file(src_id, buf, 0, (uint32_t)src_size) != src_size) {
        hw_usb_print("Error: Failed reading source file.\r\n");
        if (heap_allocated) free(buf);
        return;
    }

    if (fs_write_file(final_dst_name, buf, (uint32_t)src_size, final_dst_dir) < 0) {
        hw_usb_print("Error: Failed writing destination file.\r\n");
        if (heap_allocated) free(buf);
        return;
    }

    if (heap_allocated) {
        free(buf);
    }

    if (is_move) {
        fs_delete((uint16_t)src_id);
        hw_usb_print("File moved successfully.\r\n");
    } else {
        hw_usb_print("File copied successfully.\r\n");
    }
}

static inline uint32_t get_real_cpu_mhz(void)
{
    uint32_t pcr = *(volatile uint32_t *)0x60096118UL;

    if (pcr & (1UL << 16)) {
        return 120;
    } else if (((pcr >> 8) & 0xFF) == 1) {
        return 80;
    } else {
        return 160;
    }
}

void boot_manager_shell(const openc6_abi_t *abi)
{
    /* Newlib buffers stdout by default; disable buffering to prevent fs_list_dir truncation */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    hw_usb_print("\r\n==================================================\r\n");
    hw_usb_print("           OpenC6 Micro UNIX Shell\r\n");
    hw_usb_print("==================================================\r\n");

    if (sandbox_init_dynamic(abi, SANDBOX_DEFAULT_KERNEL_RESERVE)) {
        char msg[64];
        snprintf(msg, sizeof(msg), "[SANDBOX] Dynamic Arena ready: %zu KB\r\n", sandbox_get_arena_size() / 1024);
        hw_usb_print(msg);
    } else {
        hw_usb_print("[SANDBOX] Warning: Failed to allocate dynamic arena!\r\n");
    }

    /* Initialize process control table and preemption primitives */
    proc_manager_init(abi);

    hw_usb_print("Type 'help' for commands. 'poweroff' to shut down.\r\n\r\n");

    char input[128];
    uint16_t current_dir = 0;

    while (1) {
        char prompt[32];
        snprintf(prompt, sizeof(prompt), "openc6_fs [Dir: %d] /> ", current_dir);
        hw_usb_print(prompt);

        int idx = 0;
        memset(input, 0, sizeof(input));

        while (idx < (int)sizeof(input) - 1) {
            management_engine_pet_watchdog();
            if (hw_usb_has_data()) {
                char c = hw_usb_getc();

                if (c == '\r' || c == '\n') {
                    hw_usb_print("\r\n");
                    break;
                } else if (c == '\b' || c == 0x7F) {
                    if (idx > 0) {
                        idx--;
                        input[idx] = '\0';
                        hw_usb_print("\b \b");
                    }
                } else {
                    hw_usb_putc((uint8_t)c);
                    hw_usb_flush();
                    input[idx++] = c;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        input[idx] = '\0';

        if (strlen(input) == 0) continue;

        char *cmd = strtok(input, " ");
        if (!cmd) continue;

        if (strcmp(cmd, "help") == 0) {
            hw_usb_print("System Commands:\r\n");
            hw_usb_print("  mem                              - Display memory and sandbox statistics\r\n");
            hw_usb_print("  info                             - Display CPU frequency, temp, and ME status\r\n");
            hw_usb_print("  boot <path> [bg]                 - Execute payload in foreground or background\r\n");
            hw_usb_print("  top [kill <pid>]                 - Display active processes or kill by PID\r\n");
            hw_usb_print("  kill <pid>                       - Forcibly terminate background/suspended task\r\n");
            hw_usb_print("  suspend [pid]                    - Suspend running foreground or background job\r\n");
            hw_usb_print("  fg <pid>                         - Bring background job to foreground\r\n");
            hw_usb_print("  bg <pid>                         - Resume suspended job in background\r\n");
            hw_usb_print("  pxe <url>                        - Download payload over Wi-Fi\r\n");
            hw_usb_print("  serial [path]                    - Stream file to FS over USB (default: /downloaded/payload.bin)\r\n");
            hw_usb_print("  poweroff / exit                  - Shut down system (S5 Soft-Off)\r\n");
            hw_usb_print("  reboot                           - Reboot system\r\n");
            hw_usb_print("Network Commands:\r\n");
            hw_usb_print("  wifi scan                        - Scan for 2.4 GHz wireless networks\r\n");
            hw_usb_print("  wifi connect <ssid> [pass]       - Connect to AP and save to NVRAM\r\n");
            hw_usb_print("  wifi status                      - Display IP and RF signal status\r\n");
            hw_usb_print("  wifi disconnect                  - Disconnect station interface\r\n");
            hw_usb_print("File System Commands:\r\n");
            hw_usb_print("  ls [path]                        - List directory contents\r\n");
            hw_usb_print("  cd <path>                        - Change current directory\r\n");
            hw_usb_print("  mkdir <path>                     - Create directory\r\n");
            hw_usb_print("  cat <path>                       - Print file contents\r\n");
            hw_usb_print("  write <path> <text>              - Write text to file\r\n");
            hw_usb_print("  cp <src> <dst>                   - Copy file\r\n");
            hw_usb_print("  mv <src> <dst>                   - Move file\r\n");
            hw_usb_print("  rm <path>                        - Delete file\r\n");
            hw_usb_print("  format                           - Format file system\r\n");
        }
        else if (strcmp(cmd, "mem") == 0) {
            char out[128];
            snprintf(out, sizeof(out), "Dynamic Arena : %zu KB\r\nFree DRAM     : %zu KB\r\n",
                     sandbox_get_arena_size() / 1024,
                     heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024);
            hw_usb_print(out);
        }
        else if (strcmp(cmd, "info") == 0) {
            uint32_t mhz = get_real_cpu_mhz();
            int32_t temp = ulp_me_temperature;
            uint32_t busy = ulp_me_hp_busy_ticks;
            uint32_t idle = ulp_me_hp_idle_ticks;

            char out[224];
            snprintf(out, sizeof(out),
                     "System Health:\r\n"
                     "  Hardware CPU Clock : %lu MHz\r\n"
                     "  Governor Load      : %lu%%\r\n"
                     "  CPU Busy / Idle    : %lu / %lu ticks\r\n"
                     "  Die Junction Temp  : %ld deg C\r\n"
                     "  Management Engine  : %s\r\n",
                     (unsigned long)mhz,
                     (unsigned long)ulp_me_cpu_load,
                     (unsigned long)busy,
                     (unsigned long)idle,
                     (long)temp,
                     ulp_me_hp_is_awake ? "ACTIVE (WDT LINKED)" : "IDLE");
            hw_usb_print(out);
        }
        else if (strcmp(cmd, "poweroff") == 0 || strcmp(cmd, "exit") == 0) {
            hw_usb_print("Entering S5 Soft-Off state (Powering down)...\r\n");
            proc_kill_all();
            sandbox_deinit();
            vTaskDelay(pdMS_TO_TICKS(200));
            bios_enter_s5_state();
        }
        else if (strcmp(cmd, "reboot") == 0) {
            hw_usb_print("Rebooting system...\r\n");
            proc_kill_all();
            sandbox_deinit();
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        }
        else if (strcmp(cmd, "wifi") == 0) {
            char *subcmd = strtok(NULL, " ");
            if (!subcmd) {
                hw_usb_print("Usage: wifi <scan|connect|status|disconnect>\r\n");
                continue;
            }

            if (strcmp(subcmd, "scan") == 0) {
                wifi_mgmt_scan();
            }
            else if (strcmp(subcmd, "status") == 0) {
                wifi_mgmt_print_status();
            }
            else if (strcmp(subcmd, "disconnect") == 0) {
                wifi_mgmt_disconnect();
            }
            else if (strcmp(subcmd, "connect") == 0) {
                char *ssid = strtok(NULL, " ");
                char *pass = strtok(NULL, " ");
                if (!ssid) {
                    hw_usb_print("Usage: wifi connect <ssid> [password]\r\n");
                    continue;
                }
                wifi_mgmt_connect(ssid, pass ? pass : "", true);
            }
            else {
                hw_usb_print("Unknown wifi sub-command. Type 'help'.\r\n");
            }
        }
        else if (strcmp(cmd, "pxe") == 0) {
            char *url = strtok(NULL, " ");

            if (!url) {
                hw_usb_print("Usage: pxe <url>\r\n");
                continue;
            }

            if (!wifi_mgmt_is_connected()) {
                hw_usb_print("[PXE] Connecting to Wi-Fi via NVRAM...\r\n");
                if (wifi_mgmt_start_sta() != ESP_OK) {
                    hw_usb_print("[PXE] Error: Wi-Fi offline. Use 'wifi connect <ssid> [pass]' first.\r\n");
                    continue;
                }
            }

            if (!sandbox_get_arena()) {
                sandbox_init_dynamic(abi, SANDBOX_DEFAULT_KERNEL_RESERVE);
            }

            hw_usb_print("[PXE] Downloading payload...\r\n");
            if (pxe_boot_execute(url)) {
                hw_usb_print("[PXE] Download succeeded! File stored in /downloaded/payload.bin\r\n");
            } else {
                hw_usb_print("[PXE] Download failed.\r\n");
            }
        }
        else if (strcmp(cmd, "serial") == 0) {
            char *path_arg = strtok(NULL, " ");

            if (!sandbox_get_arena()) {
                sandbox_init_dynamic(abi, SANDBOX_DEFAULT_KERNEL_RESERVE);
            }

            size_t max_sz = sandbox_get_arena_size();
            uint8_t *arena = sandbox_get_arena();

            if (!arena || max_sz == 0) {
                hw_usb_print("Error: Buffer arena unavailable.\r\n");
                continue;
            }

            uint16_t target_dir = current_dir;
            char target_file[18] = "payload.bin";

            if (!path_arg || strlen(path_arg) == 0) {
                int16_t dl_dir = fs_find_id("downloaded", 0);
                if (dl_dir < 0) {
                    dl_dir = fs_mkdir("downloaded", 0);
                }
                if (dl_dir < 0) {
                    target_dir = 0;
                } else {
                    target_dir = (uint16_t)dl_dir;
                }
                strncpy(target_file, "payload.bin", sizeof(target_file));
            } else {
                char res_name[18];
                int16_t p_dir = shell_resolve_path(path_arg, current_dir, res_name);
                if (p_dir < 0) {
                    hw_usb_print("Error: Invalid destination path.\r\n");
                    continue;
                }
                if (strlen(res_name) == 0) {
                    target_dir = (uint16_t)p_dir;
                    strncpy(target_file, "payload.bin", sizeof(target_file));
                } else {
                    int16_t check_id = fs_find_id(res_name, (uint16_t)p_dir);
                    if (check_id >= 0 && fs_get_type(check_id) == TYPE_DIR) {
                        target_dir = (uint16_t)check_id;
                        strncpy(target_file, "payload.bin", sizeof(target_file));
                    } else {
                        target_dir = (uint16_t)p_dir;
                        strncpy(target_file, res_name, sizeof(target_file) - 1);
                        target_file[sizeof(target_file) - 1] = '\0';
                    }
                }
            }

            char info_msg[128];
            snprintf(info_msg, sizeof(info_msg),
                     "[SERIAL] Ready. Stream payload from PC over Type-C (Target: %s)...\r\n",
                     target_file);
            hw_usb_print(info_msg);

            uint32_t received = 0;
            if (shell_serial_receive(arena, max_sz, &received)) {
                if (fs_write_file(target_file, arena, received, target_dir) >= 0) {
                    if (!path_arg || strlen(path_arg) == 0) {
                        hw_usb_print("[SERIAL] Download succeeded! File stored in /downloaded/payload.bin\r\n");
                    } else {
                        hw_usb_print("[SERIAL] File written successfully.\r\n");
                    }
                } else {
                    hw_usb_print("Error: Failed to write file to flash.\r\n");
                }
            }
        }
        else if (strcmp(cmd, "boot") == 0) {
            char *path_arg = strtok(NULL, " ");
            if (!path_arg) {
                hw_usb_print("Usage: boot <path> [bg]\r\n");
                continue;
            }

            /* Detect background modifier: 'boot load.bin bg' or 'boot load.bin &' */
            bool run_in_bg = false;
            char *mode_arg = strtok(NULL, " ");
            if (mode_arg && (strcmp(mode_arg, "bg") == 0 || strcmp(mode_arg, "&") == 0)) {
                run_in_bg = true;
            }

            /* Delegate execution, PMP isolation, task creation and Aura handling to proc_manager */
            proc_spawn(path_arg, current_dir, run_in_bg, NULL);
        }

        else if (strcmp(cmd, "top") == 0) {
            char *subcmd = strtok(NULL, " ");
            if (subcmd && strcmp(subcmd, "kill") == 0) {
                char *pid_str = strtok(NULL, " ");
                if (!pid_str) {
                    hw_usb_print("Usage: top kill <pid>\r\n");
                    continue;
                }
                uint16_t pid = (uint16_t)strtoul(pid_str, NULL, 10);
                proc_kill(pid);
            } else {
                proc_print_top();
            }
        }
        else if (strcmp(cmd, "kill") == 0) {
            char *pid_str = strtok(NULL, " ");
            if (!pid_str) {
                hw_usb_print("Usage: kill <pid>\r\n");
                continue;
            }
            uint16_t pid = (uint16_t)strtoul(pid_str, NULL, 10);
            proc_kill(pid);
        }
        else if (strcmp(cmd, "fg") == 0) {
            char *pid_str = strtok(NULL, " ");
            if (!pid_str) {
                hw_usb_print("Usage: fg <pid>\r\n");
                continue;
            }
            uint16_t pid = (uint16_t)strtoul(pid_str, NULL, 10);
            proc_resume_fg(pid);
        }
        else if (strcmp(cmd, "bg") == 0) {
            char *pid_str = strtok(NULL, " ");
            if (!pid_str) {
                hw_usb_print("Usage: bg <pid>\r\n");
                continue;
            }
            uint16_t pid = (uint16_t)strtoul(pid_str, NULL, 10);
            proc_resume_bg(pid);
        }
        else if (strcmp(cmd, "suspend") == 0) {
            char *pid_str = strtok(NULL, " ");
            if (!pid_str) {
                if (proc_is_fg_active()) {
                    proc_suspend_fg();
                } else {
                    hw_usb_print("Usage: suspend <pid>\r\n");
                }
                continue;
            }
            uint16_t pid = (uint16_t)strtoul(pid_str, NULL, 10);
            if (proc_suspend(pid) != ESP_OK) {
                hw_usb_print("Error: Process not found or not running.\r\n");
            }
        }
        else if (strcmp(cmd, "write") == 0) {
            char *arg1 = strtok(NULL, " ");
            if (!arg1) {
                hw_usb_print("Usage: write <path> <text>\r\n");
                continue;
            }

            char *path_str = arg1;
            if (strcmp(arg1, "-f") == 0) {
                path_str = strtok(NULL, " ");
                if (!path_str) {
                    hw_usb_print("Usage: write <path> <text>\r\n");
                    continue;
                }
            }

            /* Capture full remainder of input buffer after path as text body */
            char *text_content = strtok(NULL, "");
            if (!text_content || strlen(text_content) == 0) {
                hw_usb_print("Error: Empty text body.\r\n");
                continue;
            }

            while (*text_content == ' ') {
                text_content++;
            }

            char name[18];
            int16_t p_dir = shell_resolve_path(path_str, current_dir, name);
            if (p_dir < 0 || strlen(name) == 0) {
                hw_usb_print("Error: Invalid path.\r\n");
                continue;
            }

            if (fs_write_file(name, (const uint8_t *)text_content, (uint32_t)strlen(text_content), (uint16_t)p_dir) >= 0) {
                hw_usb_print("File written successfully.\r\n");
            } else {
                hw_usb_print("Error: Write failed.\r\n");
            }
        }
        else if (strcmp(cmd, "cp") == 0) {
            shell_cmd_cp_mv(current_dir, false);
        }
        else if (strcmp(cmd, "mv") == 0) {
            shell_cmd_cp_mv(current_dir, true);
        }
        else if (strcmp(cmd, "format") == 0) {
            fs_format();
            current_dir = 0;
            hw_usb_print("File system formatted.\r\n");
        }
        else if (strcmp(cmd, "ls") == 0) {
            char *path_arg = strtok(NULL, " ");
            uint16_t target_dir = current_dir;
            if (path_arg) {
                char name[18];
                int16_t resolved = shell_resolve_path(path_arg, current_dir, name);
                if (resolved >= 0) {
                    if (strlen(name) == 0) {
                        target_dir = (uint16_t)resolved;
                    } else {
                        int16_t id = fs_find_id(name, (uint16_t)resolved);
                        if (id >= 0 && fs_get_type(id) == TYPE_DIR) {
                            target_dir = (uint16_t)id;
                        } else {
                            hw_usb_print("Error: Directory not found.\r\n");
                            continue;
                        }
                    }
                } else {
                    hw_usb_print("Error: Invalid path.\r\n");
                    continue;
                }
            }

            fs_list_dir(target_dir);
            fflush(stdout);
        }
        else if (strcmp(cmd, "mkdir") == 0) {
            char *arg = strtok(NULL, " ");
            if (!arg) { hw_usb_print("Usage: mkdir <path>\r\n"); continue; }

            char name[18];
            int16_t p_dir = shell_resolve_path(arg, current_dir, name);
            if (p_dir >= 0 && strlen(name) > 0) {
                if (fs_mkdir(name, (uint16_t)p_dir) >= 0) hw_usb_print("Directory created.\r\n");
                else hw_usb_print("Error: Failed to create directory.\r\n");
            }
        }
        else if (strcmp(cmd, "cat") == 0) {
            char *arg = strtok(NULL, " ");
            if (!arg) { hw_usb_print("Usage: cat <path>\r\n"); continue; }

            char name[18];
            int16_t p_dir = shell_resolve_path(arg, current_dir, name);
            if (p_dir < 0 || strlen(name) == 0) {
                hw_usb_print("Error: Invalid path.\r\n");
                continue;
            }

            int16_t id = fs_find_id(name, (uint16_t)p_dir);
            if (id >= 0 && fs_get_type(id) == TYPE_FILE) {
                int32_t file_size = fs_get_size(id);
                uint8_t chunk[64];
                uint32_t offset = 0;
                while (offset < (uint32_t)file_size) {
                    uint32_t read_len = ((uint32_t)file_size - offset > sizeof(chunk)) ? sizeof(chunk) : ((uint32_t)file_size - offset);
                    int32_t r_bytes = fs_read_file(id, chunk, offset, read_len);
                    if (r_bytes <= 0) break;
                    for (int32_t b = 0; b < r_bytes; b++) hw_usb_putc(chunk[b]);
                    hw_usb_flush();
                    offset += (uint32_t)r_bytes;
                }
                hw_usb_print("\r\n");
            } else {
                hw_usb_print("Error: File not found.\r\n");
            }
        }
        else if (strcmp(cmd, "cd") == 0) {
            char *arg = strtok(NULL, " ");
            if (!arg) { hw_usb_print("Usage: cd <path>\r\n"); continue; }

            char name[18];
            int16_t target_dir = shell_resolve_path(arg, current_dir, name);
            if (target_dir >= 0) {
                if (strlen(name) == 0) {
                    current_dir = (uint16_t)target_dir;
                } else {
                    int16_t id = fs_find_id(name, (uint16_t)target_dir);
                    if (id >= 0 && fs_get_type(id) == TYPE_DIR) current_dir = (uint16_t)id;
                    else hw_usb_print("Error: Directory not found.\r\n");
                }
            }
        }
        else if (strcmp(cmd, "rm") == 0) {
            char *arg = strtok(NULL, " ");
            if (!arg) { hw_usb_print("Usage: rm <path>\r\n"); continue; }

            char name[18];
            int16_t p_dir = shell_resolve_path(arg, current_dir, name);
            if (p_dir >= 0 && strlen(name) > 0) {
                int16_t id = fs_find_id(name, (uint16_t)p_dir);
                if (id >= 0) fs_delete((uint16_t)id);
                else hw_usb_print("Error: Object not found.\r\n");
            }
        }
        else {
            hw_usb_print("Unknown command. Type 'help'.\r\n");
        }
    }
}
