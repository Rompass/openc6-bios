#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "rom/ets_sys.h"
#include "boot_manager.h"
#include "me_shared.h"
#include "led_mgmt.h"
#include "openc6_abi.h"
#include "esp_random.h"
#include "psa/crypto.h"
#include <math.h>
#include "esp_flash.h"
#include "nvram.h"
#include "wifi_mgmt.h"

// File system integration
#include "openc6_fs.h"
#include "hal_flash.h"

// Modern ESP-IDF 5.x/6.x UART VFS drivers
#include "driver/uart_vfs.h"

#ifndef MALLOC_CAP_EXEC
#error "RISC-V Memory Protection is ENABLED! You must disable CONFIG_ESP_SYSTEM_MEMPROT_FEATURE in menuconfig to allow RAM execution."
#endif

static const char *TAG = "BOOT_MGR";

#define PIN_BTN_BOOT 9

// Synchronization Protocol Commands
#define CMD_ACK     0x06
#define CMD_EOT     0x04
#define CMD_NAK     0x15

// ─────────────────────────────────────────────────────────────────────────────
#define USE_EXTERNAL_CP2102 1
#define EXTERNAL_UART_TX_PIN 18  // <--- RE-ROUTED PHYSICAL TX PIN
#define EXTERNAL_UART_RX_PIN 19  // <--- RE-ROUTED PHYSICAL RX PIN
// ─────────────────────────────────────────────────────────────────────────────

// 1. Create small wrappers for ABI type compatibility
static void bios_delay_ms(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static void* bios_malloc(uint32_t size) {
    return malloc((size_t)size);
}

// ─── CONSOLE PRINT WRAPPER (PRINT) ──────────────────────────────────────────────

// Output text directly to the active BIOS console for debugging inside payloads
static void bios_print(const char *str) {
    printf("%s", str);
}

static void bios_sha256(const uint8_t *input, uint32_t len, uint8_t *output) {
    psa_crypto_init();
    size_t hash_len;
    psa_hash_compute(PSA_ALG_SHA_256, input, len, output, 32, &hash_len);
}

// ─── WI-FI ABI WRAPPERS ──────────────────────────────────────────────────

static int32_t bios_wifi_connect(const char* ssid, const char* pass) {
    nvram_set_wifi_sta_config(ssid, pass);
    return (wifi_mgmt_start_sta() == ESP_OK) ? 0 : -1;
}

static int32_t bios_wifi_start_ap(const char* ssid, const char* pass) {
    return (wifi_mgmt_start_ap(ssid, pass) == ESP_OK) ? 0 : -1;
}

static int32_t bios_wifi_is_connected(void) {
    return wifi_mgmt_is_connected() ? 1 : 0;
}

// ─── SYSTEM AND MEMORY METRICS ABI WRAPPERS ────────────────────────────────────

static uint32_t bios_get_free_ram(void) {
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static uint32_t bios_get_total_ram(void) {
    return heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static uint32_t bios_get_total_flash(void) {
    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        return flash_size;
    }
    return 0;
}

// ─── MATHEMATICAL COMPATIBILITY ABI WRAPPERS ────────────────────────

static uint32_t bios_math_isqrt(uint32_t x) {
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
    return res;
}

static int32_t bios_math_sin_deg(int32_t deg) {
    double rad = deg * (3.14159265359 / 180.0);
    return (int32_t)(sin(rad) * 10000.0);
}

static int32_t bios_math_cos_deg(int32_t deg) {
    double rad = deg * (3.14159265359 / 180.0);
    return (int32_t)(cos(rad) * 10000.0);
}

static void abi_fs_write_file(const char *name, const uint8_t *data, uint32_t len, uint32_t parent_id, uint8_t force) {
    fs_write_file(name, data, len, (uint16_t)parent_id);
}

static int32_t abi_fs_read_file(const char *name, uint8_t *dest, uint32_t offset, uint32_t len, uint32_t parent_id) {
    int16_t id = fs_find_id(name, (uint16_t)parent_id);
    if (id < 0) return -1;
    return fs_read_file((uint16_t)id, dest, offset, len);
}

static void abi_fs_delete(const char *name, uint32_t parent_id) {
    int16_t id = fs_find_id(name, (uint16_t)parent_id);
    if (id >= 0) {
        fs_delete((uint16_t)id);
    }
}

static const openc6_abi_t bios_abi = {
    .magic = OPENC6_ABI_MAGIC,
    .version = OPENC6_ABI_VERSION,
    .sys_reset = esp_restart,
    .set_led_color = led_mgmt_set_color,
    .delay_ms = bios_delay_ms,
    .malloc = bios_malloc,
    .free = free,
    .print = bios_print,
    .get_random = esp_random,
    .sha256 = bios_sha256,
    .math_isqrt = bios_math_isqrt,
    .math_sin_deg = bios_math_sin_deg,
    .math_cos_deg = bios_math_cos_deg,
    .wifi_connect = bios_wifi_connect,
    .wifi_start_ap = bios_wifi_start_ap,
    .wifi_is_connected = bios_wifi_is_connected,
    .get_free_ram = bios_get_free_ram,
    .get_total_ram = bios_get_total_ram,
    .get_total_flash = bios_get_total_flash,

    // File System ABI exports
    .fs_write_file = abi_fs_write_file,
    .fs_read_file = abi_fs_read_file,
    .fs_delete = abi_fs_delete
};

// ─── PORT DEFINITIONS & MACROS ───────────────────────────────────────────────
#if USE_EXTERNAL_CP2102
#define PORT_NUM UART_NUM_1
#define PORT_READ(buf, len)         uart_read_bytes(PORT_NUM, buf, len, 0)
#define PORT_WRITE(buf, len)        uart_write_bytes(PORT_NUM, buf, len)
#define PORT_FLUSH()                uart_flush_input(PORT_NUM)
#else
#define PORT_READ(buf, len)         usb_serial_jtag_read_bytes(buf, len, 0)
#define PORT_WRITE(buf, len)        usb_serial_jtag_write_bytes(buf, len, 0)
#define PORT_FLUSH()
#endif

static void bios_serial_init(void) {
    #if USE_EXTERNAL_CP2102
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(PORT_NUM, &uart_config);
    uart_set_pin(PORT_NUM, EXTERNAL_UART_TX_PIN, EXTERNAL_UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(PORT_NUM, 1024, 1024, 0, NULL, 0);
    ESP_LOGI(TAG, "Standard UART1 Driver activated. TX=%d, RX=%d", EXTERNAL_UART_TX_PIN, EXTERNAL_UART_RX_PIN);
    #else
    usb_serial_jtag_driver_config_t usj_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb_serial_jtag_driver_install(&usj_cfg);
    ESP_LOGI(TAG, "Standard USB_SERIAL_JTAG Driver activated.");
    #endif
}

static void bios_serial_deinit(void) {
    #if USE_EXTERNAL_CP2102
    uart_driver_delete(PORT_NUM);
    #endif
}

static void blink_menu(boot_option_t opt) {
    ESP_LOGI(TAG, "-> Current Option: [%d] %s", opt,
             opt == BOOT_OPT_NETWORK ? "Network Boot (PXE)" :
             opt == BOOT_OPT_SERIAL_RAM  ? "Serial Boot (RAM)" :
             opt == BOOT_OPT_SERIAL_FLASH ? "UNIX Shell (FS)" :
             opt == BOOT_OPT_DEFAULT ? "Default OS (Flash)" : "BIOS Setup");
    led_mgmt_set_aura_mode(AURA_DISABLED);
    led_mgmt_blink_post(0, 255, 255, opt + 1);
}

boot_option_t boot_manager_interactive_menu(void) {
    ESP_LOGW(TAG, "=== INTERACTIVE BOOT MENU ===");
    ESP_LOGI(TAG, "Short press (<500ms): Next Option");
    ESP_LOGI(TAG, "Long press  (>1sec) : Select Option");

    gpio_reset_pin((gpio_num_t)PIN_BTN_BOOT);
    gpio_set_direction((gpio_num_t)PIN_BTN_BOOT, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)PIN_BTN_BOOT, GPIO_PULLUP_ONLY);

    boot_option_t current_opt = BOOT_OPT_NETWORK;
    blink_menu(current_opt);

    while (1) {
        if (gpio_get_level((gpio_num_t)PIN_BTN_BOOT) == 0) {
            uint32_t press_duration_ms = 0;
            while (gpio_get_level((gpio_num_t)PIN_BTN_BOOT) == 0) {
                vTaskDelay(pdMS_TO_TICKS(10));
                press_duration_ms += 10;
            }
            if (press_duration_ms >= 1000) {
                ESP_LOGW(TAG, "Option Selected: %d", current_opt);
                led_mgmt_set_color(0, 255, 0);
                vTaskDelay(pdMS_TO_TICKS(500));
                return current_opt;
            } else if (press_duration_ms >= 50) {
                current_opt = (boot_option_t)((current_opt + 1) % BOOT_OPT_MAX);
                blink_menu(current_opt);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ─── MICRO UNIX SHELL (FS EXPLORER & LAUNCHER) ───────────────────────────────
static int16_t shell_resolve_path(const char *path, uint16_t current_dir, char *out_name) {
    uint16_t walk_dir = current_dir;
    char temp[128];
    strncpy(temp, path, 127);
    temp[127] = '\0';

    if (temp[0] == '/') walk_dir = 0; // Start at root

    char *token = strtok(temp, "/");
    char *last_token = NULL;

    while (token != NULL) {
        if (last_token != NULL) {
            if (strcmp(last_token, "..") == 0) {
                walk_dir = fs_get_parent_id(walk_dir);
            } else if (strcmp(last_token, ".") != 0) {
                int16_t next_id = fs_find_id(last_token, walk_dir);
                if (next_id >= 0 && fs_get_type(next_id) == TYPE_DIR) walk_dir = next_id;
                else return -1; // Invalid path
            }
        }
        last_token = token;
        token = strtok(NULL, "/");
    }

    if (!last_token) { out_name[0] = '\0'; return walk_dir; }

    if (strcmp(last_token, "..") == 0) {
        walk_dir = fs_get_parent_id(walk_dir);
        out_name[0] = '\0';
    } else if (strcmp(last_token, ".") == 0) {
        out_name[0] = '\0';
    } else {
        strncpy(out_name, last_token, 17);
        out_name[17] = '\0';
    }
    return walk_dir;
}

void boot_manager_shell(void) {
    ESP_LOGW(TAG, ">>> ENTERING MICRO UNIX SHELL (REDIRECTED TO UART1) <<<");

    bios_serial_init();
    uart_vfs_dev_use_driver(PORT_NUM);
    uart_vfs_dev_port_set_rx_line_endings(PORT_NUM, ESP_LINE_ENDINGS_CR);
    uart_vfs_dev_port_set_tx_line_endings(PORT_NUM, ESP_LINE_ENDINGS_CRLF);

    FILE *old_stdin = stdin;
    FILE *old_stdout = stdout;
    FILE *old_stderr = stderr;

    FILE *uart1_stream = fopen("/dev/uart/1", "r+");
    if (uart1_stream) {
        stdin = uart1_stream;
        stdout = uart1_stream;
        stderr = uart1_stream;
    }

    printf("\n==================================================\n");
    printf("        OpenC6 Micro UNIX Shell             \n");
    printf(" Type 'help' for commands. 'exit' to reboot.\n");
    printf("==================================================\n");

    char input[128];
    uint16_t current_dir = 0;

    while (1) {
        printf("\nopenc6_fs [Dir ID: %d] /> ", current_dir);
        fflush(stdout);

        int idx = 0;
        memset(input, 0, sizeof(input));
        while (idx < sizeof(input) - 1) {
            int c = getchar();
            if (c == EOF) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
            if (c == '\r' || c == '\n') {
                PORT_WRITE((const uint8_t*)"\r\n", 2);
                break;
            } else if (c == '\b' || c == 0x7F) {
                if (idx > 0) {
                    idx--; input[idx] = '\0';
                    PORT_WRITE((const uint8_t*)"\b \b", 3);
                }
            } else {
                uint8_t echo_b = (uint8_t)c;
                PORT_WRITE(&echo_b, 1);
                input[idx++] = (char)c;
            }
        }
        input[idx] = '\0';

        if (strlen(input) == 0) continue;

        char *cmd = strtok(input, " ");
        if (!cmd) continue;

        if (strcmp(cmd, "help") == 0) {
            printf("Commands:\n");
            printf("  format                  - Format file system\n");
            printf("  ls                      - List files in current directory\n");
            printf("  cd <path>               - Change directory\n");
            printf("  mkdir <path>            - Create directory\n");
            printf("  write [-f] <path> <txt> - Write text to file\n");
            printf("  cp <src> <dst_path>     - Copy file\n");
            printf("  mv <src> <dst_path>     - Move / Rename file\n");
            printf("  cat <path>              - Read text file\n");
            printf("  rm <path>               - Delete file or directory\n");
            printf("  boot <ram|xip> <path>   - Execute binary payload\n");
            printf("  exit                    - Exit shell and reboot system\n");
        }
        else if (strcmp(cmd, "exit") == 0) {
            printf("Rebooting system...\n");
            vTaskDelay(pdMS_TO_TICKS(500));
            stdin = old_stdin; stdout = old_stdout; stderr = old_stderr;
            if (uart1_stream) fclose(uart1_stream);
            bios_serial_deinit();
            esp_restart();
        }
        else if (strcmp(cmd, "format") == 0) {
            fs_format();
            current_dir = 0;
            printf("File system formatted.\n");
        }
        else if (strcmp(cmd, "ls") == 0) {
            char *arg = strtok(NULL, " ");
            if (!arg) {
                fs_list_dir(current_dir);
            } else {
                char name[18];
                int16_t p_dir = shell_resolve_path(arg, current_dir, name);
                if (p_dir < 0) {
                    printf("Error: Invalid path.\n");
                } else if (strlen(name) == 0) {
                    fs_list_dir(p_dir);
                } else {
                    int16_t id = fs_find_id(name, p_dir);
                    if (id >= 0 && fs_get_type(id) == TYPE_DIR) {
                        fs_list_dir(id);
                    } else {
                        printf("Error: Directory '%s' not found.\n", name);
                    }
                }
            }
        }
        else if (strcmp(cmd, "mkdir") == 0) {
            char *arg = strtok(NULL, " ");
            if (!arg) { printf("Usage: mkdir <path>\n"); continue; }

            char name[18];
            int16_t p_dir = shell_resolve_path(arg, current_dir, name);
            if (p_dir >= 0 && strlen(name) > 0) {
                if (fs_mkdir(name, p_dir) >= 0) printf("Directory created.\n");
                else printf("Error: Failed to create directory.\n");
            } else {
                printf("Error: Invalid path.\n");
            }
        }
        else if (strcmp(cmd, "write") == 0) {
            char *arg = strtok(NULL, " ");
            bool force = false;
            if (arg && strcmp(arg, "-f") == 0) {
                force = true;
                arg = strtok(NULL, " ");
            }
            char *text = strtok(NULL, "");

            if (!arg || !text) { printf("Usage: write [-f] <path> <text>\n"); continue; }

            char name[18];
            int16_t p_dir = shell_resolve_path(arg, current_dir, name);
            if (p_dir >= 0 && strlen(name) > 0) {
                int16_t existing_id = fs_find_id(name, p_dir);
                int32_t existing_size = 0;

                if (existing_id >= 0 && !force) {
                    if (fs_get_type(existing_id) == TYPE_DIR) {
                        printf("Error: Cannot write to a directory.\n");
                        continue;
                    }
                    existing_size = fs_get_size(existing_id);
                    if (existing_size < 0) existing_size = 0;
                }

                uint32_t append_len = strlen(text);
                uint32_t total_len = existing_size + append_len;

                uint8_t *buf = malloc(total_len > 0 ? total_len : 1);
                if (buf) {
                    if (existing_size > 0) {
                        fs_read_file(existing_id, buf, 0, existing_size);
                    }
                    memcpy(buf + existing_size, text, append_len);

                    if (fs_write_file(name, buf, total_len, p_dir) >= 0) {
                        printf("%s %lu bytes (Total: %lu bytes).\n",
                               existing_size > 0 ? "Appended" : "Written",
                               (unsigned long)append_len, (unsigned long)total_len);
                    } else {
                        printf("Error: Failed to write.\n");
                    }
                    free(buf);
                } else {
                    printf("Error: Out of memory!\n");
                }
            } else {
                printf("Error: Invalid path.\n");
            }
        }
        else if (strcmp(cmd, "cp") == 0 || strcmp(cmd, "mv") == 0) {
            bool is_move = (strcmp(cmd, "mv") == 0);
            char *src_arg = strtok(NULL, " ");
            char *dst_arg = strtok(NULL, " ");

            if (!src_arg || !dst_arg) { printf("Usage: %s <src> <dst>\n", is_move ? "mv" : "cp"); continue; }

            char src_name[18], dst_name[18];
            int16_t src_dir = shell_resolve_path(src_arg, current_dir, src_name);
            int16_t dst_dir = shell_resolve_path(dst_arg, current_dir, dst_name);

            if (src_dir < 0 || strlen(src_name) == 0) { printf("Error: Invalid source path.\n"); continue; }
            if (dst_dir < 0) { printf("Error: Invalid destination path.\n"); continue; }

            int16_t src_id = fs_find_id(src_name, src_dir);
            if (src_id < 0) { printf("Error: Source file not found.\n"); continue; }
            if (fs_get_type(src_id) != TYPE_FILE) { printf("Error: Cannot move/copy directories.\n"); continue; }

            int32_t file_size = fs_get_size(src_id);
            if (file_size < 0) continue;

            uint16_t target_dir = dst_dir;
            char target_name[18];
            strncpy(target_name, dst_name, 17); target_name[17] = '\0';

            if (strlen(dst_name) == 0) {
                strcpy(target_name, src_name);
            } else {
                int16_t dst_id = fs_find_id(dst_name, dst_dir);
                if (dst_id >= 0) {
                    if (fs_get_type(dst_id) == TYPE_DIR) {
                        target_dir = dst_id;
                        strcpy(target_name, src_name);
                    } else if (is_move) {
                        fs_delete(dst_id);
                    }
                }
            }

            uint8_t *buf = malloc(file_size > 0 ? file_size : 1);
            if (!buf && file_size > 0) { printf("Error: Out of memory!\n"); continue; }

            if (fs_read_file(src_id, buf, 0, file_size) == file_size) {
                if (fs_write_file(target_name, buf, file_size, target_dir) >= 0) {
                    if (is_move) fs_delete(src_id);
                    printf("Success.\n");
                } else {
                    printf("Error: Failed to write.\n");
                }
            } else {
                printf("Error: Failed to read source.\n");
            }
            free(buf);
        }
        else if (strcmp(cmd, "cat") == 0) {
            char *arg = strtok(NULL, " ");
            if (!arg) { printf("Usage: cat <path>\n"); continue; }

            char name[18];
            int16_t p_dir = shell_resolve_path(arg, current_dir, name);
            if (p_dir < 0 || strlen(name) == 0) { printf("Error: Invalid path.\n"); continue; }

            int16_t id = fs_find_id(name, p_dir);
            if (id >= 0 && fs_get_type(id) == TYPE_FILE) {
                int32_t file_size = fs_get_size(id);
                if (file_size > 0) {
                    uint8_t chunk[64];
                    uint32_t offset = 0;
                    while (offset < file_size) {
                        uint32_t read_len = (file_size - offset > sizeof(chunk)) ? sizeof(chunk) : (file_size - offset);
                        int32_t r_bytes = fs_read_file(id, chunk, offset, read_len);
                        if (r_bytes <= 0) break;
                        for (int32_t b = 0; b < r_bytes; b++) putchar(chunk[b]);
                        offset += r_bytes;
                    }
                    printf("\n");
                } else {
                    printf("File is empty.\n");
                }
            } else {
                printf("Error: File not found.\n");
            }
        }
        else if (strcmp(cmd, "cd") == 0) {
            char *arg = strtok(NULL, " ");
            if (!arg) { printf("Usage: cd <path>\n"); continue; }

            char name[18];
            int16_t target_dir = shell_resolve_path(arg, current_dir, name);
            if (target_dir < 0) { printf("Error: Invalid path.\n"); continue; }

            if (strlen(name) == 0) {
                current_dir = target_dir;
            } else {
                int16_t id = fs_find_id(name, target_dir);
                if (id >= 0 && fs_get_type(id) == TYPE_DIR) current_dir = id;
                else printf("Error: Directory not found.\n");
            }
        }
        else if (strcmp(cmd, "rm") == 0) {
            char *arg = strtok(NULL, " ");
            if (!arg) { printf("Usage: rm <path>\n"); continue; }

            char name[18];
            int16_t p_dir = shell_resolve_path(arg, current_dir, name);
            if (p_dir >= 0 && strlen(name) > 0) {
                int16_t id = fs_find_id(name, p_dir);
                if (id >= 0) fs_delete(id);
                else printf("Error: Object not found.\n");
            } else {
                printf("Error: Invalid path.\n");
            }
        }
        else if (strcmp(cmd, "boot") == 0) {
            char *mode = strtok(NULL, " ");
            char *arg = strtok(NULL, " ");

            if (!mode || !arg || (strcmp(mode, "ram") != 0 && strcmp(mode, "xip") != 0)) {
                printf("Usage: boot <ram|xip> <path>\n");
                continue;
            }

            char name[18];
            int16_t p_dir = shell_resolve_path(arg, current_dir, name);
            if (p_dir < 0 || strlen(name) == 0) { printf("Error: Invalid path.\n"); continue; }

            int16_t id = fs_find_id(name, p_dir);
            if (id >= 0 && fs_get_type(id) == TYPE_FILE) {
                int32_t file_size = fs_get_size(id);
                if (file_size > 0) {
                    if (strcmp(mode, "ram") == 0) {
                        void *payload_ram_ptr = heap_caps_malloc(file_size, MALLOC_CAP_EXEC | MALLOC_CAP_INTERNAL);
                        if (payload_ram_ptr) {
                            if (fs_read_file(id, payload_ram_ptr, 0, file_size) == file_size) {
                                printf("[BOOT] Loaded to RAM. Jumping...\n\n");
                                vTaskDelay(pdMS_TO_TICKS(100));

                                stdin = old_stdin; stdout = old_stdout; stderr = old_stderr;
                                if (uart1_stream) fclose(uart1_stream);
                                bios_serial_deinit();

                                typedef void (*payload_entry_t)(const openc6_abi_t *) __attribute__((noreturn));
                                payload_entry_t launch_payload = (payload_entry_t)payload_ram_ptr;
                                asm volatile ("fence.i");
                                launch_payload(&bios_abi);
                            } else {
                                printf("Error: Read failed.\n");
                                free(payload_ram_ptr);
                            }
                        } else {
                            printf("Error: Out of RAM.\n");
                        }
                    } else {
                        const esp_partition_t *xip_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x20, "payload_xip");
                        if (xip_part) {
                            if (file_size <= xip_part->size) {
                                printf("[BOOT] Erasing XIP partition...\n");
                                esp_partition_erase_range(xip_part, 0, (file_size + 4095) & ~4095);

                                printf("[BOOT] Deploying %ld bytes to XIP...\n", file_size);
                                uint8_t buf[1024];
                                uint32_t offset = 0;
                                bool copy_ok = true;
                                while (offset < file_size) {
                                    uint32_t chunk = (file_size - offset > sizeof(buf)) ? sizeof(buf) : (file_size - offset);
                                    if (fs_read_file(id, buf, offset, chunk) != chunk) {
                                        copy_ok = false;
                                        break;
                                    }
                                    esp_partition_write(xip_part, offset, buf, chunk);
                                    offset += chunk;
                                }

                                if (copy_ok) {
                                    const void *mapped_ptr = NULL;
                                    esp_partition_mmap_handle_t mmap_handle;
                                    if (esp_partition_mmap(xip_part, 0, file_size, ESP_PARTITION_MMAP_INST, &mapped_ptr, &mmap_handle) == ESP_OK) {
                                        printf("[BOOT] XIP mapped. Jumping...\n\n");
                                        vTaskDelay(pdMS_TO_TICKS(100));

                                        stdin = old_stdin; stdout = old_stdout; stderr = old_stderr;
                                        if (uart1_stream) fclose(uart1_stream);
                                        bios_serial_deinit();

                                        typedef void (*payload_entry_t)(const openc6_abi_t *) __attribute__((noreturn));
                                        payload_entry_t launch_payload = (payload_entry_t)mapped_ptr;
                                        asm volatile ("fence.i");
                                        launch_payload(&bios_abi);
                                    } else {
                                        printf("Error: MMAP failed.\n");
                                    }
                                } else {
                                    printf("Error: Copy to XIP partition failed.\n");
                                }
                            } else {
                                printf("Error: File too large for XIP partition (Max %lu bytes).\n", (unsigned long)xip_part->size);
                            }
                        } else {
                            printf("Error: 'payload_xip' partition not found!\n");
                        }
                    }
                } else {
                    printf("Error: File empty.\n");
                }
            } else {
                printf("Error: File not found.\n");
            }
        }
        else {
            printf("Unknown command. Type 'help'.\n");
        }
    }
}

// ─── PAYLOAD RECEIVER AND BOOTSTRAPPER ───────────────────────────────────────

bool boot_manager_serial_listen(int timeout_sec, payload_target_t target) {
    if (target == PAYLOAD_TARGET_FLASH) {
        ESP_LOGE(TAG, "Direct Flash serial streaming is disabled to protect OpenC6 FS integrity.");
        return false;
    }

    ESP_LOGW(TAG, "Entering Serial Boot Mode. Target: RAM");

    bios_serial_init();
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_log_level_set("*", ESP_LOG_NONE);
    PORT_FLUSH();

    led_mgmt_set_color(255, 0, 255); // Magenta status illumination

    uint8_t size_buf[4] = {0};
    uint32_t file_size = 0;
    uint64_t start_time = esp_timer_get_time();
    uint64_t last_sync_time = 0;
    uint64_t timeout_us = (uint64_t)timeout_sec * 1000000ULL;
    const char *fail_reason = "Unknown Error";

    int state = 0;

    while ((esp_timer_get_time() - start_time) < timeout_us) {
        uint64_t now = esp_timer_get_time();

        if (now - last_sync_time > 500000ULL) {
            const char* sync_msg = "##OPENC6_SYNC##";
            PORT_WRITE(sync_msg, strlen(sync_msg));
            last_sync_time = now;
        }

        uint8_t rx_b;
        while (PORT_READ(&rx_b, 1) > 0) {
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
        fail_reason = "Timeout waiting for PC to send Size Preamble (5A A5)";
        goto error_exit;
    }

    file_size = (uint32_t)size_buf[0] | ((uint32_t)size_buf[1] << 8) | ((uint32_t)size_buf[2] << 16) | ((uint32_t)size_buf[3] << 24);

    if (file_size == 0 || file_size > 500000) {
        uint8_t nak = CMD_NAK; PORT_WRITE(&nak, 1);
        fail_reason = "Invalid file size bounds configured";
        goto error_exit;
    }

    uint32_t alloc_size = (file_size + 3) & ~3;
    void *payload_ram_ptr = heap_caps_malloc(alloc_size, MALLOC_CAP_EXEC | MALLOC_CAP_INTERNAL);
    if (!payload_ram_ptr) {
        uint8_t nak = CMD_NAK; PORT_WRITE(&nak, 1);
        fail_reason = "Internal RAM allocation failed";
        goto error_exit;
    }

    uint8_t ack = CMD_ACK;
    PORT_WRITE(&ack, 1);

    uint8_t chunk_rx_buf[64];
    int total_received = 0;
    int chunk_accum = 0;
    uint64_t last_data_time = esp_timer_get_time();

    while (total_received < file_size) {
        int to_read = file_size - total_received;
        if (to_read > 64) to_read = 64;

        int rx_len = PORT_READ(chunk_rx_buf, to_read);

        if (rx_len > 0) {
            memcpy((uint8_t*)payload_ram_ptr + total_received, chunk_rx_buf, rx_len);

            total_received += rx_len;
            chunk_accum += rx_len;
            last_data_time = esp_timer_get_time();

            while (chunk_accum >= 64 || total_received == file_size) {
                PORT_WRITE(&ack, 1);
                chunk_accum -= 64;
                if (total_received == file_size && chunk_accum <= 0) break;
            }
        }

        if ((esp_timer_get_time() - last_data_time) > 5000000ULL) {
            fail_reason = "Timeout during data chunk stream transfer";
            goto error_exit;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }

    uint64_t eot_timeout = esp_timer_get_time();
    while ((esp_timer_get_time() - eot_timeout) < 1000000ULL) {
        uint8_t eot_buf;
        if (PORT_READ(&eot_buf, 1) > 0) {
            if (eot_buf == CMD_EOT) break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    esp_log_level_set("*", ESP_LOG_INFO);
    ESP_LOGI(TAG, "Payload Flashed (%d bytes). Target: RAM", total_received);
    led_mgmt_set_color(0, 255, 0);
    vTaskDelay(pdMS_TO_TICKS(100));

    bios_serial_deinit();

    typedef void (*payload_entry_t)(const openc6_abi_t *) __attribute__((noreturn));
    payload_entry_t launch_payload = (payload_entry_t)payload_ram_ptr;

    asm volatile ("fence.i");
    launch_payload(&bios_abi);

    while (1);

    error_exit:
    esp_log_level_set("*", ESP_LOG_INFO);
    ESP_LOGE(TAG, "CRASH: %s", fail_reason);
    bios_serial_deinit();
    return false;
}
