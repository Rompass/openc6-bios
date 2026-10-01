/**
 * @file main.c
 * @brief OpenC6 BIOS Fully Automated Deployment & Flashing Engine.
 *
 * Implements a freestanding, zero-dependency, C99-compliant Terminal User
 * Interface (TUI) based on POSIX termios and raw ANSI escape sequences.
 * Automates host dependencies, ESP-IDF v6.1 toolchain deployment,
 * BIOS firmware compilation, full flash erasure, and firmware flashing.
 *
 * @author OpenC6 Project
 * @version 2.0-ME
 * @license MIT
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <termios.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <fcntl.h>

#define ANSI_RESET        "\033[0m"
#define ANSI_BOLD         "\033[1m"
#define ANSI_BG_BLUE      "\033[44m"
#define ANSI_BG_GRAY      "\033[47m"
#define ANSI_BG_BLACK     "\033[40m"
#define ANSI_FG_BLACK     "\033[30m"
#define ANSI_FG_WHITE     "\033[37m"
#define ANSI_FG_RED       "\033[31m"
#define ANSI_FG_GREEN     "\033[32m"
#define ANSI_FG_YELLOW    "\033[33m"
#define ANSI_FG_CYAN      "\033[36m"
#define ANSI_CLEAR_SCREEN "\033[2J"
#define ANSI_CURSOR_HOME  "\033[H"
#define ANSI_HIDE_CURSOR  "\033[?25l"
#define ANSI_SHOW_CURSOR  "\033[?25h"

#define TUI_MIN_COLS 80
#define TUI_MIN_ROWS 24

typedef enum {
    PKG_PACMAN,  /**< Arch Linux, Manjaro, EndeavourOS */
    PKG_APT,     /**< Ubuntu, Debian, Pop!_OS, Mint     */
    PKG_UNKNOWN  /**< Generic Linux                     */
} package_manager_t;

typedef struct {
    char distro_name[64];
    package_manager_t pkg_mgr;
    bool is_wsl;
    char install_cmd[512]; /**< Expanded to 512 to prevent truncation of long apt commands */
} system_info_t;

static struct termios s_orig_termios;
static bool s_raw_mode_active = false;

/* Buffer holding the last captured log lines for error reporting */
#define LOG_LINES_COUNT 5
static char s_last_log_history[LOG_LINES_COUNT][256];

/* ============================================================================
 * TERMINAL CONTROL
 * ============================================================================ */

static void tui_restore_terminal(void) {
    if (s_raw_mode_active) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &s_orig_termios);
        s_raw_mode_active = false;
    }
    printf(ANSI_SHOW_CURSOR ANSI_RESET ANSI_CLEAR_SCREEN ANSI_CURSOR_HOME);
    fflush(stdout);
}

static void tui_signal_handler(int sig) {
    (void)sig;
    tui_restore_terminal();
    _exit(1);
}

static void tui_enable_raw_mode(void) {
    if (tcgetattr(STDIN_FILENO, &s_orig_termios) == -1) {
        perror("tcgetattr");
        exit(EXIT_FAILURE);
    }

    atexit(tui_restore_terminal);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = tui_signal_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    struct termios raw = s_orig_termios;
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        perror("tcsetattr");
        exit(EXIT_FAILURE);
    }
    s_raw_mode_active = true;
    printf(ANSI_HIDE_CURSOR);
    fflush(stdout);
}

static bool tui_get_window_size(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        return false;
    }
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return true;
}

static inline void tui_move_cursor(int row, int col) {
    printf("\033[%d;%dH", row, col);
}

static void tui_draw_background(int rows, int cols) {
    printf(ANSI_BG_BLUE);
    for (int r = 1; r <= rows; r++) {
        tui_move_cursor(r, 1);
        for (int c = 1; c <= cols; c++) {
            putchar(' ');
        }
    }
}

/**
 * @brief Renders dialog box at explicitly designated screen coordinates.
 */
static void tui_draw_dialog_ex(const char *title, int width, int height, int start_row, int start_col) {
    /* 1. Draw Gray Dialog Body */
    printf(ANSI_BG_GRAY ANSI_FG_BLACK);
    for (int r = 0; r < height; r++) {
        tui_move_cursor(start_row + r, start_col);
        for (int c = 0; c < width; c++) {
            putchar(' ');
        }
    }

    /* 2. Draw Borders */
    tui_move_cursor(start_row, start_col);
    printf("┌");
    for (int i = 0; i < width - 2; i++) printf("─");
    printf("┐");

    for (int r = 1; r < height - 1; r++) {
        tui_move_cursor(start_row + r, start_col);
        printf("│");
        tui_move_cursor(start_row + r, start_col + width - 1);
        printf("│");
    }

    tui_move_cursor(start_row + height - 1, start_col);
    printf("└");
    for (int i = 0; i < width - 2; i++) printf("─");
    printf("┘");

    /* 3. Centered Title */
    int title_len = (int)strlen(title);
    int title_pos = start_col + (width - title_len - 4) / 2;
    tui_move_cursor(start_row, title_pos);
    printf("┤ " ANSI_BOLD "%s" ANSI_RESET ANSI_BG_GRAY ANSI_FG_BLACK " ├", title);

    /* 4. Draw Shadows with strict boundary coordinates */
    printf(ANSI_RESET ANSI_BG_BLACK);

    /* Right Shadow */
    for (int r = 1; r <= height; r++) {
        tui_move_cursor(start_row + r, start_col + width);
        printf("  ");
    }

    /* Bottom Shadow */
    tui_move_cursor(start_row + height, start_col + 2);
    for (int c = 0; c < width; c++) {
        putchar(' ');
    }

    /* Reset back to gray background */
    printf(ANSI_RESET ANSI_BG_GRAY ANSI_FG_BLACK);
    fflush(stdout);
}

/**
 * @brief Renders centered dialog box on the screen.
 */
static void tui_draw_dialog(const char *title, int width, int height, int *out_start_row, int *out_start_col) {
    int rows, cols;
    tui_get_window_size(&rows, &cols);

    int start_row = (rows - height) / 2;
    int start_col = (cols - width) / 2;

    if (out_start_row) *out_start_row = start_row;
    if (out_start_col) *out_start_col = start_col;

    tui_draw_dialog_ex(title, width, height, start_row, start_col);
}

/* ============================================================================
 * SYSTEM OPERATIONS & RUNNERS
 * ============================================================================ */

static void sys_detect_distribution(system_info_t *info) {
    memset(info, 0, sizeof(system_info_t));
    info->pkg_mgr = PKG_UNKNOWN;
    strncpy(info->distro_name, "Generic Linux", sizeof(info->distro_name) - 1);

    bool is_root = (geteuid() == 0);
    const char *sudo_prefix = is_root ? "" : "sudo ";

    if (getenv("MOCK_WSL") != NULL) {
        info->is_wsl = true;
    } else {
        FILE *ver_file = fopen("/proc/version", "r");
        if (ver_file) {
            char buf[256];
            if (fgets(buf, sizeof(buf), ver_file)) {
                if (strstr(buf, "Microsoft") || strstr(buf, "microsoft") || strstr(buf, "WSL")) {
                    info->is_wsl = true;
                }
            }
            fclose(ver_file);
        }
    }

    FILE *os_file = fopen("/etc/os-release", "r");
    if (!os_file) {
        strncpy(info->install_cmd, "Install git, cmake, ninja, python3 manually.", sizeof(info->install_cmd) - 1);
        return;
    }

    char line[256];
    char id[64] = {0};
    char id_like[64] = {0};
    char pretty[64] = {0};

    while (fgets(line, sizeof(line), os_file)) {
        if (strncmp(line, "PRETTY_NAME=", 12) == 0) {
            char *val = line + 12;
            if (*val == '"') val++;
            size_t len = strlen(val);
            if (len > 0 && val[len - 1] == '\n') val[--len] = '\0';
            if (len > 0 && val[len - 1] == '"') val[--len] = '\0';
            strncpy(pretty, val, sizeof(pretty) - 1);
        } else if (strncmp(line, "ID=", 3) == 0) {
            sscanf(line + 3, "%63s", id);
        } else if (strncmp(line, "ID_LIKE=", 8) == 0) {
            sscanf(line + 8, "%63s", id_like);
        }
    }
    fclose(os_file);

    if (strlen(pretty) > 0) {
        strncpy(info->distro_name, pretty, sizeof(info->distro_name) - 1);
    }

    if (strcmp(id, "arch") == 0 || strcmp(id, "manjaro") == 0 || strcmp(id, "endeavouros") == 0 || strstr(id_like, "arch")) {
        info->pkg_mgr = PKG_PACMAN;
        snprintf(info->install_cmd, sizeof(info->install_cmd),
                 "%spacman -Sy --noconfirm base-devel git wget cmake ninja python python-pip",
                 sudo_prefix);
    } else if (strcmp(id, "ubuntu") == 0 || strcmp(id, "debian") == 0 || strcmp(id, "pop") == 0 || strcmp(id, "linuxmint") == 0 ||
        strstr(id_like, "debian") || strstr(id_like, "ubuntu")) {
        info->pkg_mgr = PKG_APT;
    /* Comprehensive Ubuntu dependencies: includes python3-pip, python-is-python3, libffi-dev and libssl-dev */
    snprintf(info->install_cmd, sizeof(info->install_cmd),
             "%sDEBIAN_FRONTEND=noninteractive apt-get update && "
             "%sDEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "
             "build-essential git wget cmake ninja-build python3 python3-pip python3-venv "
             "python-is-python3 libffi-dev libssl-dev libusb-1.0-0 dfu-util",
             sudo_prefix, sudo_prefix);
        } else {
            info->pkg_mgr = PKG_UNKNOWN;
            strncpy(info->install_cmd, "Ensure build-essential/base-devel, git, cmake, ninja, python3, pip, and venv are installed.", sizeof(info->install_cmd) - 1);
        }
}

static bool sys_check_tool_installed(const char *binary) {
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "command -v %s >/dev/null 2>&1", binary);
    return (system(cmd) == 0);
}

static void sanitize_log_line(char *str) {
    char *src = str;
    char *dst = str;
    while (*src) {
        if (*src == '\033') {
            src++;
            if (*src == '[') {
                src++;
                while (*src && (*src < '@' || *src > '~')) {
                    src++;
                }
                if (*src) src++;
            }
        } else if (*src == '\r' || *src == '\n') {
            src++;
        } else if ((unsigned char)*src < 32 && *src != '\t') {
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/**
 * @brief Executes command while rendering an aesthetic bottom console log window.
 */
static bool run_task_with_log_box(const char *task_title, const char *command) {
    int rows, cols;
    tui_get_window_size(&rows, &cols);

    tui_draw_background(rows, cols);

    /* 1. Upper status dialog: positioned at row 3 */
    int top_height = 9;
    int top_width = 74;
    int top_row = 3;
    int top_col = (cols - top_width) / 2;

    tui_draw_dialog_ex("OpenC6 Execution Manager", top_width, top_height, top_row, top_col);

    tui_move_cursor(top_row + 2, top_col + 4);
    printf(ANSI_BG_GRAY ANSI_FG_BLACK ANSI_BOLD "Task: %s" ANSI_RESET ANSI_BG_GRAY ANSI_FG_BLACK, task_title);

    tui_move_cursor(top_row + 4, top_col + 4);
    printf("Processing command stream. Monitor live output in the console below.");

    tui_move_cursor(top_row + 6, top_col + 4);
    printf(ANSI_BG_GRAY ANSI_FG_CYAN ANSI_BOLD "Status: ACTIVE (Do not interrupt)..." ANSI_RESET ANSI_BG_GRAY ANSI_FG_BLACK);
    fflush(stdout);

    /* 2. Lower Black Console Log Window (Bottom 7 rows) */
    int log_h = 7;
    int log_w = cols - 8;
    int log_r = rows - log_h - 1;
    int log_c = 5;

    /* Frame and background fill: 100% black inside to avoid any blue leakage */
    tui_move_cursor(log_r, log_c);
    printf(ANSI_BG_BLACK ANSI_FG_CYAN "┌");
    for (int i = 0; i < log_w - 2; i++) printf("─");
    printf("┐");

    for (int r = 1; r < log_h - 1; r++) {
        tui_move_cursor(log_r + r, log_c);
        printf(ANSI_BG_BLACK ANSI_FG_CYAN "│");
        printf(ANSI_BG_BLACK);
        for (int c = 0; c < log_w - 2; c++) {
            putchar(' ');
        }
        printf(ANSI_BG_BLACK ANSI_FG_CYAN "│");
    }

    tui_move_cursor(log_r + log_h - 1, log_c);
    printf(ANSI_BG_BLACK ANSI_FG_CYAN "└");
    for (int i = 0; i < log_w - 2; i++) printf("─");
    printf("┘");

    /* Header for console */
    tui_move_cursor(log_r, log_c + 4);
    printf(ANSI_BG_BLACK ANSI_FG_CYAN "┤ " ANSI_BOLD "Terminal Console Output" ANSI_RESET ANSI_BG_BLACK ANSI_FG_CYAN " ├");
    fflush(stdout);

    /* Pipe execution */
    char full_cmd[512];
    snprintf(full_cmd, sizeof(full_cmd), "%s 2>&1", command);

    FILE *fp = popen(full_cmd, "r");
    if (!fp) {
        return false;
    }

    for (int i = 0; i < LOG_LINES_COUNT; i++) memset(s_last_log_history[i], 0, sizeof(s_last_log_history[i]));

    int text_width = log_w - 4;
    char raw_line[512];

    while (fgets(raw_line, sizeof(raw_line), fp)) {
        sanitize_log_line(raw_line);
        if (strlen(raw_line) == 0) continue;

        for (int i = 0; i < LOG_LINES_COUNT - 1; i++) {
            strncpy(s_last_log_history[i], s_last_log_history[i + 1], sizeof(s_last_log_history[i]) - 1);
        }
        strncpy(s_last_log_history[LOG_LINES_COUNT - 1], raw_line, sizeof(s_last_log_history[0]) - 1);

        /* Render lines strictly filling interior between borders */
        printf(ANSI_BG_BLACK ANSI_FG_GREEN);
        for (int i = 0; i < LOG_LINES_COUNT; i++) {
            tui_move_cursor(log_r + 1 + i, log_c + 1);
            printf(" %-*.*s ", text_width, text_width, s_last_log_history[i]);
        }
        fflush(stdout);
    }

    int status = pclose(fp);
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return (exit_code == 0);
}

static bool sys_find_esp_port(char *out_port, size_t max_len) {
    DIR *d = opendir("/dev");
    if (!d) return false;

    struct dirent *dir;
    while ((dir = readdir(d)) != NULL) {
        if (strncmp(dir->d_name, "ttyACM", 6) == 0 || strncmp(dir->d_name, "ttyUSB", 6) == 0) {
            snprintf(out_port, max_len, "/dev/%s", dir->d_name);
            closedir(d);
            return true;
        }
    }
    closedir(d);
    return false;
}

/* ============================================================================
 * INTERACTIVE SCREENS
 * ============================================================================ */

static bool screen_welcome(const system_info_t *info) {
    int start_row, start_col;
    tui_draw_dialog("OpenC6 BIOS v2.0-ME Setup", 74, 18, &start_row, &start_col);

    tui_move_cursor(start_row + 2, start_col + 4);
    printf(ANSI_BG_GRAY ANSI_FG_BLACK ANSI_BOLD "Welcome to the OpenC6 Automated Deployment Wizard!" ANSI_RESET ANSI_BG_GRAY ANSI_FG_BLACK);

    tui_move_cursor(start_row + 4, start_col + 4);
    printf("Host OS       : %s", info->distro_name);

    tui_move_cursor(start_row + 5, start_col + 4);
    printf("Package Mgr   : %s", info->pkg_mgr == PKG_PACMAN ? "pacman (Arch Linux)" :
    info->pkg_mgr == PKG_APT ? "apt (Debian/Ubuntu)" : "Generic Linux (Manual)");

    tui_move_cursor(start_row + 6, start_col + 4);
    printf("Environment   : %s", info->is_wsl ? ANSI_FG_RED "WSL2 (Windows Subsystem for Linux)" ANSI_FG_BLACK : "Native Linux");

    tui_move_cursor(start_row + 8, start_col + 4);
    printf("This installer will verify dependencies, deploy ESP-IDF v6.1,");
    tui_move_cursor(start_row + 9, start_col + 4);
    printf("compile the host BIOS firmware, erase flash, and install OpenC6.");

    int selected_button = 0;

    while (1) {
        tui_move_cursor(start_row + 14, start_col + 16);
        if (selected_button == 0) {
            printf(ANSI_BG_BLUE ANSI_FG_WHITE ANSI_BOLD "[ Start Setup ]" ANSI_RESET ANSI_BG_GRAY ANSI_FG_BLACK);
        } else {
            printf("[ Start Setup ]");
        }

        tui_move_cursor(start_row + 14, start_col + 42);
        if (selected_button == 1) {
            printf(ANSI_BG_BLUE ANSI_FG_WHITE ANSI_BOLD "[ Cancel ]" ANSI_RESET ANSI_BG_GRAY ANSI_FG_BLACK);
        } else {
            printf("[ Cancel ]");
        }
        fflush(stdout);

        char c = (char)getchar();
        if (c == '\033') {
            getchar();
            char arrow = (char)getchar();
            if (arrow == 'C' || arrow == 'D') {
                selected_button = 1 - selected_button;
            }
        } else if (c == '\t') {
            selected_button = 1 - selected_button;
        } else if (c == '\n' || c == '\r' || c == ' ') {
            return (selected_button == 0);
        } else if (c == 'q' || c == 'Q') {
            return false;
        }
    }
}

static bool screen_check_dependencies(const system_info_t *info) {
    bool has_git   = sys_check_tool_installed("git");
    bool has_cmake = sys_check_tool_installed("cmake");
    bool has_ninja = sys_check_tool_installed("ninja");
    bool has_py3   = sys_check_tool_installed("python3");

    bool all_satisfied = has_git && has_cmake && has_ninja && has_py3;

    int start_row, start_col;
    tui_draw_dialog("Host Prerequisites Audit", 74, 18, &start_row, &start_col);

    tui_move_cursor(start_row + 2, start_col + 4);
    printf(ANSI_BG_GRAY ANSI_FG_BLACK ANSI_BOLD "Checking host development utilities:" ANSI_RESET ANSI_BG_GRAY ANSI_FG_BLACK);

    tui_move_cursor(start_row + 4, start_col + 6);
    printf("git          : [%s]", has_git ? ANSI_FG_GREEN "INSTALLED" ANSI_FG_BLACK : ANSI_FG_RED "MISSING" ANSI_FG_BLACK);

    tui_move_cursor(start_row + 5, start_col + 6);
    printf("cmake        : [%s]", has_cmake ? ANSI_FG_GREEN "INSTALLED" ANSI_FG_BLACK : ANSI_FG_RED "MISSING" ANSI_FG_BLACK);

    tui_move_cursor(start_row + 6, start_col + 6);
    printf("ninja        : [%s]", has_ninja ? ANSI_FG_GREEN "INSTALLED" ANSI_FG_BLACK : ANSI_FG_RED "MISSING" ANSI_FG_BLACK);

    tui_move_cursor(start_row + 7, start_col + 6);
    printf("python3      : [%s]", has_py3 ? ANSI_FG_GREEN "INSTALLED" ANSI_FG_BLACK : ANSI_FG_RED "MISSING" ANSI_FG_BLACK);

    if (all_satisfied) {
        tui_move_cursor(start_row + 10, start_col + 4);
        printf(ANSI_FG_GREEN "All required host build dependencies are satisfied!" ANSI_FG_BLACK);

        tui_move_cursor(start_row + 14, start_col + 28);
        printf(ANSI_BG_BLUE ANSI_FG_WHITE ANSI_BOLD "[ Continue ]" ANSI_RESET);
        fflush(stdout);

        while (1) {
            char c = (char)getchar();
            if (c == '\n' || c == '\r' || c == ' ') return true;
        }
    } else {
        tui_move_cursor(start_row + 9, start_col + 4);
        printf(ANSI_FG_RED "Missing packages detected. Command to install:" ANSI_FG_BLACK);

        tui_move_cursor(start_row + 11, start_col + 4);
        printf(ANSI_BOLD "%.66s" ANSI_RESET ANSI_BG_GRAY ANSI_FG_BLACK, info->install_cmd);

        tui_move_cursor(start_row + 14, start_col + 24);
        printf(ANSI_BG_BLUE ANSI_FG_WHITE ANSI_BOLD "[ Auto-Install Now ]" ANSI_RESET);
        fflush(stdout);

        while (1) {
            char c = (char)getchar();
            if (c == '\n' || c == '\r' || c == ' ') {
                if (info->pkg_mgr == PKG_UNKNOWN) {
                    return true;
                }
                tui_restore_terminal();

                int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
                fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
                while (getchar() != EOF);
                fcntl(STDIN_FILENO, F_SETFL, flags);

                printf("\nExecuting installation command:\n%s\n\n", info->install_cmd);
                int ret = system(info->install_cmd);
                tui_enable_raw_mode();
                return (ret == 0);
            } else if (c == 'q' || c == 'Q') {
                return false;
            }
        }
    }
}

static bool step_deploy_idf(void) {
    char idf_dir[256];
    const char *home = getenv("HOME");
    snprintf(idf_dir, sizeof(idf_dir), "%s/esp-idf", home ? home : ".");

    struct stat st;
    if (stat(idf_dir, &st) != 0) {
        char clone_cmd[512];
        snprintf(clone_cmd, sizeof(clone_cmd),
                 "git clone -b release/v6.1 --depth 1 --shallow-submodules "
                 "https://github.com/espressif/esp-idf.git %s", idf_dir);

        if (!run_task_with_log_box("Cloning ESP-IDF v6.1 Framework", clone_cmd)) {
            return false;
        }
    }

    char install_tools_cmd[512];
    snprintf(install_tools_cmd, sizeof(install_tools_cmd),
             "bash -c 'cd %s && ./install.sh esp32c6'", idf_dir);

    return run_task_with_log_box("Installing RISC-V Toolchain (ESP32-C6 Target)", install_tools_cmd);
}

static bool step_build_openc6(void) {
    char build_cmd[512];
    const char *home = getenv("HOME");
    snprintf(build_cmd, sizeof(build_cmd),
             "bash -c 'source %s/esp-idf/export.sh >/dev/null 2>&1 && idf.py build'",
             home ? home : ".");

    return run_task_with_log_box("Compiling OpenC6 BIOS Firmware", build_cmd);
}

static bool screen_hardware_guide(const system_info_t *info, char *selected_port, size_t max_port_len) {
    int start_row, start_col;
    tui_draw_dialog("Hardware Connection & Download Mode", 74, 18, &start_row, &start_col);

    tui_move_cursor(start_row + 2, start_col + 4);
    printf(ANSI_BG_GRAY ANSI_FG_BLACK ANSI_BOLD "Put your ESP32-C6 into Hardware Download Mode:" ANSI_RESET ANSI_BG_GRAY ANSI_FG_BLACK);

    tui_move_cursor(start_row + 4, start_col + 4);
    printf("1. " ANSI_BOLD "UNPLUG" ANSI_RESET ANSI_BG_GRAY ANSI_FG_BLACK " the USB Type-C cable from your board.");

    tui_move_cursor(start_row + 5, start_col + 4);
    printf("2. " ANSI_BOLD "PRESS AND HOLD" ANSI_RESET ANSI_BG_GRAY ANSI_FG_BLACK " the physical [ BOOT ] button (GPIO 9).");

    tui_move_cursor(start_row + 6, start_col + 4);
    printf("3. " ANSI_BOLD "PLUG IN" ANSI_RESET ANSI_BG_GRAY ANSI_FG_BLACK " the USB Type-C cable into your workstation.");

    tui_move_cursor(start_row + 7, start_col + 4);
    printf("4. " ANSI_BOLD "RELEASE" ANSI_RESET ANSI_BG_GRAY ANSI_FG_BLACK " the [ BOOT ] button after 1 second.");

    if (info->is_wsl) {
        tui_move_cursor(start_row + 9, start_col + 4);
        printf(ANSI_FG_RED "[!] WSL2 Detected:" ANSI_FG_BLACK " Attach USB device from PowerShell (Admin):");
        tui_move_cursor(start_row + 10, start_col + 8);
        printf(ANSI_BOLD "usbipd attach --wsl --busid <BUSID>" ANSI_RESET ANSI_BG_GRAY ANSI_FG_BLACK);
    }

    tui_move_cursor(start_row + 14, start_col + 22);
    printf(ANSI_BG_BLUE ANSI_FG_WHITE ANSI_BOLD "[ Detect Port & Flash BIOS ]" ANSI_RESET);
    fflush(stdout);

    while (1) {
        char c = (char)getchar();
        if (c == '\n' || c == '\r' || c == ' ') {
            if (sys_find_esp_port(selected_port, max_port_len)) {
                return true;
            } else {
                tui_move_cursor(start_row + 12, start_col + 14);
                printf(ANSI_FG_RED ANSI_BOLD "No ESP32-C6 port detected! Check cable & retry." ANSI_RESET ANSI_BG_GRAY ANSI_FG_BLACK);
                fflush(stdout);
            }
        }
        if (c == 'q' || c == 'Q') return false;
    }
}

static bool step_erase_flash(const char *port) {
    char erase_cmd[512];
    const char *home = getenv("HOME");
    bool is_root = (geteuid() == 0);

    if (access(port, W_OK) != 0 && !is_root) {
        char perm_cmd[128];
        snprintf(perm_cmd, sizeof(perm_cmd), "sudo chmod 666 %s", port);
        tui_restore_terminal();
        printf("\nGranting write permissions to serial port %s:\n", port);
        system(perm_cmd);
        tui_enable_raw_mode();
    }

    snprintf(erase_cmd, sizeof(erase_cmd),
             "bash -c 'source %s/esp-idf/export.sh >/dev/null 2>&1 && idf.py -p %s erase-flash'",
             home ? home : ".", port);

    return run_task_with_log_box("Sanitizing SPI Flash (erase-flash)", erase_cmd);
}

static bool step_flash_firmware(const char *port) {
    char flash_cmd[512];
    const char *home = getenv("HOME");

    snprintf(flash_cmd, sizeof(flash_cmd),
             "bash -c 'source %s/esp-idf/export.sh >/dev/null 2>&1 && idf.py -p %s flash'",
             home ? home : ".", port);

    return run_task_with_log_box("Writing OpenC6 Firmware to Flash", flash_cmd);
}

static void screen_success(const char *port) {
    int start_row, start_col;
    tui_draw_dialog("OpenC6 Successfully Deployed", 74, 16, &start_row, &start_col);

    tui_move_cursor(start_row + 2, start_col + 4);
    printf(ANSI_BG_GRAY ANSI_FG_GREEN ANSI_BOLD "OpenC6 BIOS v2.0-ME is now installed on your ESP32-C6!" ANSI_RESET ANSI_BG_GRAY ANSI_FG_BLACK);

    tui_move_cursor(start_row + 4, start_col + 4);
    printf("Open your serial terminal to access the shell:");

    tui_move_cursor(start_row + 6, start_col + 8);
    printf(ANSI_BOLD "idf.py -p %s monitor" ANSI_RESET ANSI_BG_GRAY ANSI_FG_BLACK, port);

    tui_move_cursor(start_row + 8, start_col + 4);
    printf("------------------------------------------------------------------");

    tui_move_cursor(start_row + 10, start_col + 14);
    printf(ANSI_BG_GRAY ANSI_FG_CYAN ANSI_BOLD "Good luck! Let's build the future together!" ANSI_RESET ANSI_BG_GRAY ANSI_FG_BLACK);

    tui_move_cursor(start_row + 13, start_col + 30);
    printf(ANSI_BG_BLUE ANSI_FG_WHITE ANSI_BOLD "[ Finish ]" ANSI_RESET);
    fflush(stdout);

    while (1) {
        char c = (char)getchar();
        if (c == '\n' || c == '\r' || c == ' ' || c == 'q' || c == 'Q') break;
    }
}

/**
 * @brief Dumps last captured task logs to stderr upon abnormal termination.
 */
static void dump_last_error_log(const char *failed_step) {
    tui_restore_terminal();
    fprintf(stderr, "\n============================================================\n");
    fprintf(stderr, "[-] INSTALLATION ERROR at step: %s\n", failed_step);
    fprintf(stderr, "[-] Last terminal console output:\n");
    fprintf(stderr, "------------------------------------------------------------\n");
    for (int i = 0; i < LOG_LINES_COUNT; i++) {
        if (strlen(s_last_log_history[i]) > 0) {
            fprintf(stderr, "  %s\n", s_last_log_history[i]);
        }
    }
    fprintf(stderr, "============================================================\n\n");
}

/* ============================================================================
 * MAIN LIFECYCLE
 * ============================================================================ */

int main(void) {
    int rows, cols;
    if (!tui_get_window_size(&rows, &cols) || cols < TUI_MIN_COLS || rows < TUI_MIN_ROWS) {
        fprintf(stderr, "Error: Terminal window is too small!\n");
        fprintf(stderr, "OpenC6 Installer requires at least %dx%d (Current: %dx%d).\n",
                TUI_MIN_COLS, TUI_MIN_ROWS, cols, rows);
        return EXIT_FAILURE;
    }

    system_info_t sys_info;
    sys_detect_distribution(&sys_info);

    tui_enable_raw_mode();
    tui_draw_background(rows, cols);

    /* 1. Welcome Screen */
    if (!screen_welcome(&sys_info)) {
        return EXIT_SUCCESS;
    }

    /* 2. Check & Install Host Packages */
    tui_draw_background(rows, cols);
    if (!screen_check_dependencies(&sys_info)) {
        return EXIT_FAILURE;
    }

    /* 3. Clone & Install ESP-IDF v6.1 */
    if (!step_deploy_idf()) {
        dump_last_error_log("ESP-IDF v6.1 Deployment & Toolchain Setup");
        return EXIT_FAILURE;
    }

    /* 4. Compile OpenC6 BIOS */
    if (!step_build_openc6()) {
        dump_last_error_log("Firmware Compilation (idf.py build)");
        return EXIT_FAILURE;
    }

    /* 5. Prompt for Download Mode & Auto-detect Port */
    char target_port[64] = {0};
    tui_draw_background(rows, cols);
    if (!screen_hardware_guide(&sys_info, target_port, sizeof(target_port))) {
        return EXIT_SUCCESS;
    }

    /* 6. Sanitize SPI Flash Memory (Erase Chip) */
    if (!step_erase_flash(target_port)) {
        dump_last_error_log("SPI Flash Erasure (erase-flash)");
        return EXIT_FAILURE;
    }

    /* 7. Flash Firmware to Hardware */
    if (!step_flash_firmware(target_port)) {
        dump_last_error_log("Firmware Flashing (idf.py flash)");
        return EXIT_FAILURE;
    }

    /* 8. Success Banner */
    tui_draw_background(rows, cols);
    screen_success(target_port);

    return EXIT_SUCCESS;
}
