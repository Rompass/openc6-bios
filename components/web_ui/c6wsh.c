#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "web_ui.h"
#include "proc_manager.h"
#include "led_mgmt.h"
#include "hw_usb.h"
#include "openc6_fs.h"
#include "sandbox.h"
#include "wifi_mgmt.h"
#include "pxe_boot.h"
#include "nvram.h"
#include "management_engine.h"
#include "me_shared.h"

static const char *TAG = "C6WSH";

/* 4 KB circular ring buffer for console telemetry stream */
#define C6WSH_RING_BUFFER_SIZE  4096U

static char s_ring_buffer[C6WSH_RING_BUFFER_SIZE];
static uint32_t s_write_head = 0;
static portMUX_TYPE s_c6wsh_mux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_data_ready_sem = NULL;
static QueueHandle_t s_cmd_queue = NULL;
static httpd_handle_t s_c6wsh_server = NULL;
static bool s_c6wsh_active = false;
static const openc6_abi_t *s_abi = NULL;

/* Current working directory sector ID tracked across HTTP requests */
static uint16_t s_current_dir = 0;

/* External entry point to trigger hardware S5 deep sleep */
extern void bios_enter_s5_state(void);

/* Embedded full-screen retro UNIX terminal HTML document */
static const char s_c6wsh_html[] =
"<!DOCTYPE html>"
"<html><head><meta charset='utf-8'>"
"<title>c6wsh - OpenC6 Web Shell</title>"
"<meta name='viewport' content='width=device-width, initial-scale=1'>"
"<style>"
"body{background:#0a0a0a;color:#00ff66;font-family:'Courier New',monospace;margin:0;padding:10px;display:flex;flex-direction:column;height:95vh;box-sizing:border-box}"
"#hdr{border-bottom:1px solid #005522;padding-bottom:5px;margin-bottom:5px;display:flex;justify-content:space-between;font-size:0.85em;color:#88ffaa}"
"#term{flex:1;background:#000000;border:1px solid #005522;padding:10px;overflow-y:auto;white-space:pre-wrap;word-break:break-all;font-size:0.9em;line-height:1.3;box-shadow:inset 0 0 10px rgba(0,255,102,0.1)}"
"#input-row{display:flex;margin-top:8px;background:#001100;border:1px solid #005522;padding:5px}"
"#prompt{color:#ffff55;font-weight:bold;margin-right:8px;user-select:none}"
"#cmd{flex:1;background:transparent;border:none;outline:none;color:#00ff66;font-family:'Courier New',monospace;font-size:1em;font-weight:bold}"
".btns{margin-top:5px;display:flex;gap:5px;flex-wrap:wrap}"
"button{background:#002200;border:1px solid #005522;color:#00ff66;font-family:'Courier New',monospace;font-size:0.8em;padding:4px 8px;cursor:pointer;font-weight:bold}"
"button:hover{background:#004400;color:#ffffff}"
"#status{font-weight:bold}"
"</style></head><body>"
"<div id='hdr'>"
"<span>OPENC6 REMOTE WEB SHELL [c6wsh v1.0]</span>"
"<span id='status' style='color:#ffff55'>CONNECTING...</span>"
"</div>"
"<div id='term'></div>"
"<div class='btns'>"
"<button onclick=\"sendCmd('help')\">HELP</button>"
"<button onclick=\"sendCmd('mem')\">MEM</button>"
"<button onclick=\"sendCmd('top')\">TOP</button>"
"<button onclick=\"sendCmd('wifi status')\">WIFI</button>"
"<button onclick=\"sendCmd('ls')\">LS</button>"
"<button onclick=\"sendCmd('info')\">INFO</button>"
"<button onclick=\"clearTerm()\">CLEAR</button>"
"<button onclick=\"sendCmd('suspend')\" style='color:#ffff55;border-color:#666600'>SUSPEND [Ctrl+X]</button>"
"<button onclick=\"sendCmd('reboot')\">REBOOT</button>"
"<button onclick=\"sendCmd('poweroff')\" style='color:#ff5555;border-color:#550000'>POWEROFF</button>"
"</div>"
"<div id='input-row'>"
"<span id='prompt'>c6wsh /></span>"
"<input type='text' id='cmd' autofocus autocomplete='off' spellcheck='false'>"
"</div>"
"<script>"
"const term=document.getElementById('term');"
"const cmdInput=document.getElementById('cmd');"
"const statusElem=document.getElementById('status');"
"let history=[];let histIdx=-1;"
"let streamTail=-1;"
"function clearTerm(){term.textContent='';}"
"async function pollStream(){"
"  let pollInterval = 500;"
"  try{"
"    const url=streamTail>=0?('/c6wsh/stream?tail='+streamTail):'/c6wsh/stream';"
"    const res=await fetch(url);"
"    if(res.ok){"
"      statusElem.textContent='STREAM ACTIVE (ONLINE)';"
"      statusElem.style.color='#00ff66';"
"      const newTail=res.headers.get('X-Tail');"
"      if(newTail!==null)streamTail=parseInt(newTail);"
"      const text=await res.text();"
"      if(text.length>0){"
"        term.textContent+=text;"
"        term.scrollTop=term.scrollHeight;"
"        pollInterval = 80;"
"      } else {"
"        pollInterval = 500;"
"      }"
"    }"
"  }catch(e){"
"    statusElem.textContent='STREAM OFFLINE (RECONNECTING...)';"
"    statusElem.style.color='#ff4444';"
"    pollInterval = 2000;"
"  }"
"  setTimeout(pollStream, pollInterval);"
"}"
"async function sendCmd(text){"
"  if(!text||!text.trim())return;"
"  try{"
"    await fetch('/c6wsh/exec',{method:'POST',headers:{'Content-Type':'text/plain'},body:text.trim()});"
"  }catch(e){"
"    term.textContent+='\\r\\n[c6wsh] Error: Command dispatch failed.\\r\\n';"
"  }"
"}"
"cmdInput.addEventListener('keydown',(e)=>{"
"  if(e.ctrlKey && (e.key === 'x' || e.key === 'X')){"
"    e.preventDefault();"
"    sendCmd('suspend');"
"    return;"
"  }"
"  if(e.key==='Enter'){"
"    const val=cmdInput.value.trim();"
"    if(!val)return;"
"    if(val==='clear'){clearTerm();cmdInput.value='';return;}"
"    history.push(val);"
"    histIdx=history.length;"
"    sendCmd(val);"
"    cmdInput.value='';"
"  }else if(e.key==='ArrowUp'){"
"    if(histIdx>0){histIdx--;cmdInput.value=history[histIdx];}"
"  }else if(e.key==='ArrowDown'){"
"    if(histIdx<history.length-1){histIdx++;cmdInput.value=history[histIdx];}else{histIdx=history.length;cmdInput.value='';}"
"  }"
"});"
"window.onload=()=>{pollStream();cmdInput.focus();};"
"</script></body></html>";

/**
 * @brief Transmits string directly to hardware USB CDC FIFO.
 * The active telemetry tap (g_hw_usb_tap) automatically duplicates it to c6wsh ring buffer.
 *
 * @param str Null-terminated ASCII string.
 */
static void c6wsh_print(const char *str)
{
    if (str) {
        hw_usb_print(str);
    }
}

/**
 * @brief Resolves relative or absolute path within the virtual directory structure.
 * Supports root '/', parent '..', and current '.' tokens.
 *
 * @param path Input path string (e.g. "dir/file" or "/file").
 * @param current_dir Current working directory sector ID.
 * @param out_name Destination buffer for extracted target entry name (minimum 18 bytes).
 * @return Parent directory sector ID on success, or -1 if path components are missing.
 */
static int16_t c6wsh_resolve_path(const char *path, uint16_t current_dir, char *out_name)
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
 * @brief Handles file duplication and relocation (cp and mv commands) via openc6_fs.
 * Resolves paths, supports directory targets, and utilizes sandbox arena as fallback buffer.
 *
 * @param saveptr Pointer to remaining command line argument string.
 * @param is_move true for move (delete source upon success), false for copy.
 */
static void c6wsh_cmd_cp_mv(char *saveptr, bool is_move)
{
    char *src_arg = strtok_r(NULL, " ", &saveptr);
    char *dst_arg = strtok_r(NULL, " ", &saveptr);
    if (!src_arg || !dst_arg) {
        c6wsh_print(is_move ? "Usage: mv <src> <dst>\r\n" : "Usage: cp <src> <dst>\r\n");
        return;
    }

    char src_name[18];
    int16_t src_pdir = c6wsh_resolve_path(src_arg, s_current_dir, src_name);
    if (src_pdir < 0 || strlen(src_name) == 0) {
        c6wsh_print("Error: Invalid source path.\r\n");
        return;
    }

    int16_t src_id = fs_find_id(src_name, (uint16_t)src_pdir);
    if (src_id < 0 || fs_get_type(src_id) != TYPE_FILE) {
        c6wsh_print("Error: Source file not found.\r\n");
        return;
    }

    int32_t src_size = fs_get_size(src_id);
    if (src_size < 0) {
        c6wsh_print("Error: Cannot read source file size.\r\n");
        return;
    }

    char dst_name[18];
    int16_t dst_pdir = c6wsh_resolve_path(dst_arg, s_current_dir, dst_name);
    if (dst_pdir < 0) {
        c6wsh_print("Error: Invalid destination path.\r\n");
        return;
    }

    uint16_t final_dst_dir = (uint16_t)dst_pdir;
    char final_dst_name[18];

    /* If target is an existing directory or ends in slash, preserve source filename */
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
            c6wsh_print(is_move ? "File moved successfully.\r\n" : "File copied successfully.\r\n");
        } else {
            c6wsh_print("Error: Write failed.\r\n");
        }
        return;
    }

    buf = (uint8_t *)malloc((size_t)src_size);
    if (!buf) {
        if (sandbox_get_arena() && (size_t)src_size <= sandbox_get_arena_size()) {
            buf = sandbox_get_arena();
        } else {
            c6wsh_print("Error: Insufficient memory for copy buffer.\r\n");
            return;
        }
    } else {
        heap_allocated = true;
    }

    if (fs_read_file(src_id, buf, 0, (uint32_t)src_size) != src_size) {
        c6wsh_print("Error: Failed reading source file.\r\n");
        if (heap_allocated) free(buf);
        return;
    }

    if (fs_write_file(final_dst_name, buf, (uint32_t)src_size, final_dst_dir) < 0) {
        c6wsh_print("Error: Failed writing destination file.\r\n");
        if (heap_allocated) free(buf);
        return;
    }

    if (heap_allocated) {
        free(buf);
    }

    if (is_move) {
        fs_delete((uint16_t)src_id);
        c6wsh_print("File moved successfully.\r\n");
    } else {
        c6wsh_print("File copied successfully.\r\n");
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
/**
 * @brief Parses and executes command string received from web client or USB console.
 * Implements complete parity with boot_shell commands (write, cp, mv, boot, poweroff).
 *
 * @param cmd_line Raw input command line.
 */
void c6wsh_execute_command(const char *cmd_line)
{
    if (!cmd_line || strlen(cmd_line) == 0) {
        return;
    }

    char echo_buf[160];
    snprintf(echo_buf, sizeof(echo_buf), "\r\nopenc6_fs [Dir: %d] /> %s\r\n", s_current_dir, cmd_line);
    c6wsh_print(echo_buf);

    char buf[160];
    strncpy(buf, cmd_line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *saveptr = NULL;
    char *cmd = strtok_r(buf, " ", &saveptr);
    if (!cmd) {
        return;
    }

    if (strcmp(cmd, "help") == 0) {
        c6wsh_print("System Commands:\r\n"
        "  mem                              - Display memory and sandbox statistics\r\n"
        "  info                             - Display CPU frequency, temp, and ME status\r\n"
        "  boot <path> [bg]                 - Execute payload in foreground or background\r\n"
        "  top [kill <pid>]                 - Display active processes or kill by PID\r\n"
        "  kill <pid>                       - Forcibly terminate background/suspended task\r\n"
        "  suspend [pid]                    - Suspend running foreground or background job\r\n"
        "  fg <pid>                         - Bring background job to foreground\r\n"
        "  bg <pid>                         - Resume suspended job in background\r\n"
        "  pxe <url>                        - Download payload over Wi-Fi\r\n"
        "  poweroff / exit                  - Shut down system (S5 Soft-Off state)\r\n"
        "  reboot                           - Reboot system\r\n"
        "Network Commands:\r\n"
        "  wifi scan                        - Scan for 2.4 GHz wireless networks\r\n"
        "  wifi connect <ssid> [pass]       - Connect to AP and save to NVRAM\r\n"
        "  wifi status                      - Display IP and RF signal status\r\n"
        "  wifi disconnect                  - Disconnect station interface\r\n"
        "File System Commands:\r\n"
        "  ls [path]                        - List directory contents\r\n"
        "  cd <path>                        - Change current directory\r\n"
        "  mkdir <path>                     - Create directory\r\n"
        "  cat <path>                       - Print file contents\r\n"
        "  write <path> <text>              - Write text to file\r\n"
        "  cp <src> <dst>                   - Copy file\r\n"
        "  mv <src> <dst>                   - Move / rename file\r\n"
        "  rm <path>                        - Delete file\r\n"
        "  format                           - Format file system\r\n");
    } else if (strcmp(cmd, "mem") == 0) {
        char out[128];
        snprintf(out, sizeof(out), "Dynamic Arena : %zu KB\r\nFree DRAM     : %zu KB\r\n",
                 sandbox_get_arena_size() / 1024,
                 heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024);
        c6wsh_print(out);
    } else if (strcmp(cmd, "info") == 0) {
        /* Read true physical CPU clock directly from silicon PCR register (0x60096118) */
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
        c6wsh_print(out);
    } else if (strcmp(cmd, "poweroff") == 0 || strcmp(cmd, "exit") == 0) {
        c6wsh_print("Entering S5 Soft-Off state (Powering down)...\r\n");
        proc_kill_all();
        sandbox_deinit();
        vTaskDelay(pdMS_TO_TICKS(200));
        bios_enter_s5_state();
    } else if (strcmp(cmd, "reboot") == 0) {
        c6wsh_print("Rebooting system...\r\n");
        proc_kill_all();
        sandbox_deinit();
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else if (strcmp(cmd, "wifi") == 0) {
        char *subcmd = strtok_r(NULL, " ", &saveptr);
        if (!subcmd) {
            c6wsh_print("Usage: wifi <scan|connect|status|disconnect>\r\n");
            return;
        }

        if (strcmp(subcmd, "scan") == 0) {
            wifi_mgmt_scan();
        } else if (strcmp(subcmd, "status") == 0) {
            wifi_mgmt_print_status();
        } else if (strcmp(subcmd, "disconnect") == 0) {
            wifi_mgmt_disconnect();
        } else if (strcmp(subcmd, "connect") == 0) {
            char *ssid = strtok_r(NULL, " ", &saveptr);
            char *pass = strtok_r(NULL, " ", &saveptr);
            if (!ssid) {
                c6wsh_print("Usage: wifi connect <ssid> [password]\r\n");
                return;
            }
            wifi_mgmt_connect(ssid, pass ? pass : "", true);
        } else {
            c6wsh_print("Unknown wifi sub-command. Type 'help'.\r\n");
        }
    } else if (strcmp(cmd, "pxe") == 0) {
        char *url = strtok_r(NULL, " ", &saveptr);
        if (!url) {
            c6wsh_print("Usage: pxe <url>\r\n");
            return;
        }

        if (!wifi_mgmt_is_connected()) {
            c6wsh_print("[PXE] Connecting to Wi-Fi via NVRAM...\r\n");
            if (wifi_mgmt_start_sta() != ESP_OK) {
                c6wsh_print("[PXE] Error: Wi-Fi offline. Use 'wifi connect <ssid> [pass]' first.\r\n");
                return;
            }
        }

        /* Ensure sandbox arena is ready before network streaming */
        if (!sandbox_get_arena()) {
            sandbox_init_dynamic(s_abi, SANDBOX_DEFAULT_KERNEL_RESERVE);
        }

        c6wsh_print("[PXE] Downloading payload...\r\n");
        if (pxe_boot_execute(url)) {
            c6wsh_print("[PXE] Download succeeded! File stored in /downloaded/payload.bin\r\n");
        } else {
            c6wsh_print("[PXE] Download failed.\r\n");
        }
    } else if (strcmp(cmd, "boot") == 0) {
        char *arg = strtok_r(NULL, " ", &saveptr);
        if (!arg) {
            c6wsh_print("Usage: boot <path> [bg]\r\n");
            return;
        }

        /* Check for background execution flag */
        bool run_in_bg = false;
        char *mode_arg = strtok_r(NULL, " ", &saveptr);
        if (mode_arg && (strcmp(mode_arg, "bg") == 0 || strcmp(mode_arg, "&") == 0)) {
            run_in_bg = true;
        }

        /* Dispatch execution to process manager */
        proc_spawn(arg, s_current_dir, run_in_bg, NULL);
    } else if (strcmp(cmd, "write") == 0) {
        char *arg1 = strtok_r(NULL, " ", &saveptr);
        if (!arg1) {
            c6wsh_print("Usage: write <path> <text>\r\n");
            return;
        }

        char *path_str = arg1;
        if (strcmp(arg1, "-f") == 0) {
            path_str = strtok_r(NULL, " ", &saveptr);
            if (!path_str) {
                c6wsh_print("Usage: write <path> <text>\r\n");
                return;
            }
        }

        /* Capture the remainder of the command line as full text body */
        char *text_content = saveptr;
        if (!text_content || strlen(text_content) == 0) {
            c6wsh_print("Error: Empty text body.\r\n");
            return;
        }

        while (*text_content == ' ') {
            text_content++;
        }

        char name[18];
        int16_t p_dir = c6wsh_resolve_path(path_str, s_current_dir, name);
        if (p_dir < 0 || strlen(name) == 0) {
            c6wsh_print("Error: Invalid path.\r\n");
            return;
        }

        if (fs_write_file(name, (const uint8_t *)text_content, (uint32_t)strlen(text_content), (uint16_t)p_dir) >= 0) {
            c6wsh_print("File written successfully.\r\n");
        } else {
            c6wsh_print("Error: Write failed.\r\n");
        }
    } else if (strcmp(cmd, "cp") == 0) {
        c6wsh_cmd_cp_mv(saveptr, false);
    } else if (strcmp(cmd, "mv") == 0) {
        c6wsh_cmd_cp_mv(saveptr, true);
    } else if (strcmp(cmd, "format") == 0) {
        fs_format();
        s_current_dir = 0;
        c6wsh_print("File system formatted.\r\n");
    } else if (strcmp(cmd, "ls") == 0) {
        char *path_arg = strtok_r(NULL, " ", &saveptr);
        uint16_t target_dir = s_current_dir;
        if (path_arg) {
            char name[18];
            int16_t resolved = c6wsh_resolve_path(path_arg, s_current_dir, name);
            if (resolved >= 0) {
                if (strlen(name) == 0) {
                    target_dir = (uint16_t)resolved;
                } else {
                    int16_t id = fs_find_id(name, (uint16_t)resolved);
                    if (id >= 0 && fs_get_type(id) == TYPE_DIR) {
                        target_dir = (uint16_t)id;
                    } else {
                        c6wsh_print("Error: Directory not found.\r\n");
                        return;
                    }
                }
            } else {
                c6wsh_print("Error: Invalid path.\r\n");
                return;
            }
        }

        fs_list_dir(target_dir);
        fflush(stdout);
    } else if (strcmp(cmd, "mkdir") == 0) {
        char *arg = strtok_r(NULL, " ", &saveptr);
        if (!arg) {
            c6wsh_print("Usage: mkdir <path>\r\n");
            return;
        }

        char name[18];
        int16_t p_dir = c6wsh_resolve_path(arg, s_current_dir, name);
        if (p_dir >= 0 && strlen(name) > 0) {
            if (fs_mkdir(name, (uint16_t)p_dir) >= 0) {
                c6wsh_print("Directory created.\r\n");
            } else {
                c6wsh_print("Error: Failed to create directory.\r\n");
            }
        }
    } else if (strcmp(cmd, "cat") == 0) {
        char *arg = strtok_r(NULL, " ", &saveptr);
        if (!arg) {
            c6wsh_print("Usage: cat <path>\r\n");
            return;
        }

        char name[18];
        int16_t p_dir = c6wsh_resolve_path(arg, s_current_dir, name);
        if (p_dir < 0 || strlen(name) == 0) {
            c6wsh_print("Error: Invalid path.\r\n");
            return;
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
                for (int32_t b = 0; b < r_bytes; b++) {
                    hw_usb_putc(chunk[b]);
                }
                c6wsh_write_output(chunk, (size_t)r_bytes);
                hw_usb_flush();
                offset += (uint32_t)r_bytes;
            }
            c6wsh_print("\r\n");
        } else {
            c6wsh_print("Error: File not found.\r\n");
        }
    } else if (strcmp(cmd, "cd") == 0) {
        char *arg = strtok_r(NULL, " ", &saveptr);
        if (!arg) {
            c6wsh_print("Usage: cd <path>\r\n");
            return;
        }

        char name[18];
        int16_t target_dir = c6wsh_resolve_path(arg, s_current_dir, name);
        if (target_dir >= 0) {
            if (strlen(name) == 0) {
                s_current_dir = (uint16_t)target_dir;
            } else {
                int16_t id = fs_find_id(name, (uint16_t)target_dir);
                if (id >= 0 && fs_get_type(id) == TYPE_DIR) {
                    s_current_dir = (uint16_t)id;
                } else {
                    c6wsh_print("Error: Directory not found.\r\n");
                }
            }
        }
    } else if (strcmp(cmd, "rm") == 0) {
        char *arg = strtok_r(NULL, " ", &saveptr);
        if (!arg) {
            c6wsh_print("Usage: rm <path>\r\n");
            return;
        }

        char name[18];
        int16_t p_dir = c6wsh_resolve_path(arg, s_current_dir, name);
        if (p_dir >= 0 && strlen(name) > 0) {
            int16_t id = fs_find_id(name, (uint16_t)p_dir);
            if (id >= 0) {
                fs_delete((uint16_t)id);
                c6wsh_print("Object deleted.\r\n");
            } else {
                c6wsh_print("Error: Object not found.\r\n");
            }
        }
    } else if (strcmp(cmd, "top") == 0) {
        char *sub = strtok_r(NULL, " ", &saveptr);
        if (sub && strcmp(sub, "kill") == 0) {
            char *pid_str = strtok_r(NULL, " ", &saveptr);
            if (!pid_str) {
                c6wsh_print("Usage: top kill <pid>\r\n");
                return;
            }
            uint16_t pid = (uint16_t)strtoul(pid_str, NULL, 10);
            proc_kill(pid);
        } else {
            proc_print_top();
        }
    } else if (strcmp(cmd, "kill") == 0) {
        char *pid_str = strtok_r(NULL, " ", &saveptr);
        if (!pid_str) {
            c6wsh_print("Usage: kill <pid>\r\n");
            return;
        }
        uint16_t pid = (uint16_t)strtoul(pid_str, NULL, 10);
        proc_kill(pid);
    } else if (strcmp(cmd, "fg") == 0) {
        char *pid_str = strtok_r(NULL, " ", &saveptr);
        if (!pid_str) {
            c6wsh_print("Usage: fg <pid>\r\n");
            return;
        }
        uint16_t pid = (uint16_t)strtoul(pid_str, NULL, 10);
        proc_resume_fg(pid);
    } else if (strcmp(cmd, "bg") == 0) {
        char *pid_str = strtok_r(NULL, " ", &saveptr);
        if (!pid_str) {
            c6wsh_print("Usage: bg <pid>\r\n");
            return;
        }
        uint16_t pid = (uint16_t)strtoul(pid_str, NULL, 10);
        proc_resume_bg(pid);
    } else if (strcmp(cmd, "suspend") == 0 || strcmp(cmd, "ctrl-x") == 0) {
        char *pid_str = strtok_r(NULL, " ", &saveptr);
        if (pid_str && strlen(pid_str) > 0) {
            uint16_t pid = (uint16_t)strtoul(pid_str, NULL, 10);
            if (proc_suspend(pid) != ESP_OK) {
                c6wsh_print("Error: Process not found or not running.\r\n");
            }
        } else {
            if (proc_is_fg_active()) {
                proc_suspend_fg();
            } else {
                c6wsh_print("Usage: suspend <pid>\r\n");
            }
        }

    } else {
        c6wsh_print("Unknown command. Type 'help'.\r\n");
    }
}

/**
 * @brief GET / - Serves the lightweight c6wsh interactive terminal HTML page.
 */
static esp_err_t c6wsh_index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, s_c6wsh_html, sizeof(s_c6wsh_html) - 1);
}

/**
 * @brief GET /c6wsh/stream - Serves available console output slices non-blockingly.
 * Avoids blocking the single-threaded HTTP worker task so /c6wsh/exec commands are serviced instantly.
 */
static esp_err_t c6wsh_stream_handler(httpd_req_t *req)
{
    char query[32] = {0};
    uint32_t client_tail = 0;
    bool has_tail = false;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[16] = {0};
        if (httpd_query_key_value(query, "tail", val, sizeof(val)) == ESP_OK) {
            client_tail = (uint32_t)strtoul(val, NULL, 10);
            has_tail = true;
        }
    }

    uint32_t cur_head;
    portENTER_CRITICAL(&s_c6wsh_mux);
    cur_head = s_write_head;
    portEXIT_CRITICAL(&s_c6wsh_mux);

    /* Fresh connections without tail receive recent 512 bytes of console history */
    if (!has_tail || (cur_head > client_tail && (cur_head - client_tail) > C6WSH_RING_BUFFER_SIZE)) {
        client_tail = (cur_head > 512U) ? (cur_head - 512U) : 0U;
    }

    char chunk_buf[512];
    size_t to_read = 0;

    if (cur_head > client_tail) {
        size_t available = (size_t)(cur_head - client_tail);
        to_read = (available > sizeof(chunk_buf)) ? sizeof(chunk_buf) : available;

        for (size_t i = 0; i < to_read; i++) {
            chunk_buf[i] = s_ring_buffer[(client_tail + i) % C6WSH_RING_BUFFER_SIZE];
        }
        client_tail += to_read;
    }

    char tail_str[16];
    snprintf(tail_str, sizeof(tail_str), "%lu", (unsigned long)client_tail);

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "X-Tail", tail_str);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache");

    if (to_read > 0) {
        return httpd_resp_send(req, chunk_buf, to_read);
    }
    return httpd_resp_send(req, "", 0);
}

/**
 * @brief POST /c6wsh/exec - Ingests remote command into execution queue.
 * Returns immediately with 200 OK so the httpd task is never blocked by long-running payloads.
 */
static esp_err_t c6wsh_exec_handler(httpd_req_t *req)
{
    char buf[128];
    int len = req->content_len;
    if (len <= 0 || len >= (int)sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid command length");
        return ESP_FAIL;
    }

    int received = httpd_req_recv(req, buf, len);
    if (received <= 0) {
        return ESP_FAIL;
    }
    buf[received] = '\0';

    /* Forward command to background runner task via FreeRTOS queue */
    if (s_cmd_queue) {
        xQueueSend(s_cmd_queue, buf, 0);
    }
    return httpd_resp_sendstr(req, "OK");
}

bool c6wsh_get_pending_command(char *dest, size_t max_len)
{
    if (!s_cmd_queue || !dest || max_len == 0) {
        return false;
    }
    char tmp[128];
    if (xQueueReceive(s_cmd_queue, tmp, 0) == pdTRUE) {
        strncpy(dest, tmp, max_len - 1);
        dest[max_len - 1] = '\0';
        return true;
    }
    return false;
}

void c6wsh_write_output(const uint8_t *data, size_t len)
{
    if (!s_c6wsh_active || !data || len == 0) {
        return;
    }

    portENTER_CRITICAL(&s_c6wsh_mux);
    for (size_t i = 0; i < len; i++) {
        s_ring_buffer[s_write_head % C6WSH_RING_BUFFER_SIZE] = (char)data[i];
        s_write_head++;
    }
    portEXIT_CRITICAL(&s_c6wsh_mux);

    if (s_data_ready_sem) {
        xSemaphoreGive(s_data_ready_sem);
    }
}

bool c6wsh_is_active(void)
{
    return s_c6wsh_active;
}

void c6wsh_stop(void)
{
    if (s_c6wsh_server) {
        s_c6wsh_active = false;
        httpd_stop(s_c6wsh_server);
        s_c6wsh_server = NULL;
    }

    if (s_data_ready_sem) {
        vSemaphoreDelete(s_data_ready_sem);
        s_data_ready_sem = NULL;
    }

    if (s_cmd_queue) {
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
    }

    ESP_LOGI(TAG, "c6wsh daemon stopped and socket resources released");
}

esp_err_t c6wsh_start(const openc6_abi_t *abi)
{
    if (s_c6wsh_active) {
        return ESP_OK;
    }

    s_abi = abi;

    /* Initialize dynamic sandbox arena and hardware PMP isolation */
    if (sandbox_init_dynamic(abi, SANDBOX_DEFAULT_KERNEL_RESERVE)) {
        char msg[80];
        snprintf(msg, sizeof(msg), "[SANDBOX] Dynamic Arena ready: %zu KB (PMP active)\r\n",
                 sandbox_get_arena_size() / 1024);
        hw_usb_print(msg);
    } else {
        hw_usb_print("[SANDBOX] Warning: Failed to allocate dynamic arena!\r\n");
    }

    /* Initialize process control table and preemption primitives */
    proc_manager_init(abi);

    if (s_cmd_queue == NULL) {
        /* Allocate a 4-slot FIFO queue for non-blocking remote command dispatch */
        s_cmd_queue = xQueueCreate(4, 128);
    }

    if (s_data_ready_sem == NULL) {
        s_data_ready_sem = xSemaphoreCreateBinary();
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32769; /* Distinct control port from BIOS Setup web server */
    config.stack_size = 12288; /* 12 KB stack required for partition XIP erase & flash writes */
    config.lru_purge_enable = true;

    ESP_LOGI(TAG, "Starting c6wsh Web Shell on port %d", config.server_port);
    esp_err_t err = httpd_start(&s_c6wsh_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed starting HTTP listener: %s", esp_err_to_name(err));
        return err;
    }

    httpd_uri_t uri_root = { .uri = "/", .method = HTTP_GET, .handler = c6wsh_index_handler };
    httpd_register_uri_handler(s_c6wsh_server, &uri_root);

    httpd_uri_t uri_alias = { .uri = "/c6wsh", .method = HTTP_GET, .handler = c6wsh_index_handler };
    httpd_register_uri_handler(s_c6wsh_server, &uri_alias);

    httpd_uri_t uri_stream = { .uri = "/c6wsh/stream", .method = HTTP_GET, .handler = c6wsh_stream_handler };
    httpd_register_uri_handler(s_c6wsh_server, &uri_stream);

    httpd_uri_t uri_exec = { .uri = "/c6wsh/exec", .method = HTTP_POST, .handler = c6wsh_exec_handler };
    httpd_register_uri_handler(s_c6wsh_server, &uri_exec);

    s_c6wsh_active = true;
    c6wsh_print("\r\n[c6wsh] Daemon started. Remote Web Shell active on port 80.\r\n");
    return ESP_OK;
}
