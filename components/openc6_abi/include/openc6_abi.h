#pragma once

#include <stdint.h>

#define OPENC6_ABI_MAGIC   0x43364249
#define OPENC6_ABI_VERSION 2

typedef struct {
    uint32_t magic;
    uint32_t version;

    // ─── SYSTEM API WRAPPERS ─────────────────────────────────────────────────
    void (*sys_reset)(void) __attribute__((noreturn));
    void (*set_led_color)(uint8_t r, uint8_t g, uint8_t b);
    void (*delay_ms)(uint32_t ms);
    void* (*malloc)(uint32_t size);
    void (*free)(void* ptr);
    void (*print)(const char *str);
    uint32_t (*get_random)(void);
    void (*sha256)(const uint8_t *input, uint32_t len, uint8_t *output);

    // ─── SYSTEM MATHEMATICAL FUNCTIONS ───────────────────────────────────────
    uint32_t (*math_isqrt)(uint32_t x);
    int32_t (*math_sin_deg)(int32_t angle_deg);
    int32_t (*math_cos_deg)(int32_t angle_deg);

    // ─── NETWORK & SOCKET INTERFACES (TCP/IP) ────────────────────────────────
    // 1 if station is connected to AP and has L3 IP, 0 otherwise
    int32_t (*wifi_is_connected)(void);

    // Retrieves current station IPv4 address string (e.g. "192.168.1.195"). Returns 0 on success.
    int32_t (*net_get_ip)(char *buf, uint32_t max_len);

    // Creates a listening TCP socket on specified port. Returns server socket fd (>=0) or -1 on error.
    int32_t (*tcp_listen)(uint16_t port);

    // Accepts incoming client connection with timeout in ms. Returns client fd (>=0) or -1 on timeout.
    int32_t (*tcp_accept)(int32_t server_fd, uint32_t timeout_ms);

    // Reads data from connected socket. Returns bytes read (>0), 0 on disconnect, -1 on error.
    int32_t (*tcp_read)(int32_t fd, void *buf, uint32_t max_len);

    // Transmits byte buffer to connected socket. Returns bytes sent (>=0) or -1 on error.
    int32_t (*tcp_write)(int32_t fd, const void *buf, uint32_t len);

    // Closes socket descriptor.
    void (*tcp_close)(int32_t fd);

    // ─── TELEMETRY AND HEAP MONITORING INTERFACES ────────────────────────────
    uint32_t (*get_free_ram)(void);
    uint32_t (*get_total_ram)(void);
    uint32_t (*get_total_flash)(void);

    // ─── HIGH-SPEED FILE SYSTEM INTERFACES ───────────────────────────────────
    void (*fs_write_file)(const char *name, const uint8_t *data, uint32_t len, uint32_t dir_sector, uint8_t force);
    int32_t (*fs_read_file)(const char *name, uint8_t *dest, uint32_t offset, uint32_t len, uint32_t dir_sector);
    void (*fs_delete)(const char *name, uint32_t dir_sector);

    // ─── HARDWARE GPIO CONTROL INTERFACES ────────────────────────────────────
    int32_t (*gpio_set_dir)(uint32_t pin, uint32_t is_output);
    int32_t (*gpio_write)(uint32_t pin, uint32_t level);
    int32_t (*gpio_read)(uint32_t pin);

} openc6_abi_t;
