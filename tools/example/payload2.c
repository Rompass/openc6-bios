/* tools/example/payload2.c
 * OpenC6 Microkernel OS: Persistent Background HTTP Daemon
 * Runs indefinitely in U-Mode under PMP protection on port 8080.
 */
#include "openc6_abi.h"

static void print_uint32(const openc6_abi_t *abi, uint32_t val) {
    char buf[16];
    int i = 14;
    buf[15] = '\0';

    if (val == 0) {
        buf[i--] = '0';
    } else {
        while (val > 0 && i >= 0) {
            buf[i--] = '0' + (val % 10);
            val /= 10;
        }
    }
    abi->print(&buf[i + 1]);
}

void *memset(void *dest, int c, uint32_t n) {
    uint8_t *d = (uint8_t *)dest;
    while (n--) *d++ = (uint8_t)c;
    return dest;
}

void *memcpy(void *dest, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dest;
}

static uint32_t str_len(const char *s) {
    uint32_t len = 0;
    while (*s++) len++;
    return len;
}

void __attribute__((section(".text.entry"), noreturn)) payload_main(const openc6_abi_t *abi) {

    if (!abi || abi->magic != OPENC6_ABI_MAGIC || abi->version != OPENC6_ABI_VERSION) {
        while (1) { __asm__ volatile("nop"); }
    }

    abi->print("\r\n==================================================\r\n");
    abi->print("   OPENC6 BACKGROUND HTTP DAEMON (payload2.bin)  \r\n");
    abi->print("==================================================\r\n");

    if (!abi->wifi_is_connected()) {
        abi->print("[DAEMON] Error: Wi-Fi offline. Connect Wi-Fi first.\r\n");
        while (1) { abi->delay_ms(1000); }
    }

    char ip_str[32];
    memset(ip_str, 0, sizeof(ip_str));
    abi->net_get_ip(ip_str, sizeof(ip_str));

    int32_t server_fd = abi->tcp_listen(8080);
    if (server_fd < 0) {
        abi->print("[DAEMON] Error: Failed to bind TCP port 8080.\r\n");
        while (1) { abi->delay_ms(1000); }
    }

    abi->print("[DAEMON] Listening on http://");
    abi->print(ip_str);
    abi->print(":8080/\r\n");
    abi->print("[DAEMON] Ready for background execution (Use 'top' to view)\r\n\r\n");

    uint32_t hit_count = 0;

    /* Continuous daemon loop: stays alive serving requests indefinitely */
    while (1) {
        int32_t client_fd = abi->tcp_accept(server_fd, 500);
        if (client_fd >= 0) {
            hit_count++;

            char req[128];
            abi->tcp_read(client_fd, req, sizeof(req) - 1);

            /* Indicator pulse on status LED */
            abi->set_led_color(0, 255, 128);

            const char *http_response =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Connection: close\r\n\r\n"
            "<!DOCTYPE html><html><head><meta charset='utf-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>OpenC6 Daemon Node</title></head>"
            "<body style='background:#050510;color:#00ffcc;font-family:monospace;padding:25px;line-height:1.5;'>"
            "<h1 style='color:#00ff66;'>OPENC6 BACKGROUND MICRO-SERVER</h1>"
            "<p style='color:#88ffcc;'>Running persistently in unprivileged <b>RISC-V U-Mode</b> under PMP isolation!</p>"
            "<div style='background:#0a0e1a;padding:15px;border:1px solid #00ff66;box-shadow:0 0 10px rgba(0,255,102,0.2);'>"
            "<p>DAEMON STATUS : <b style='color:#00ff66;'>ONLINE (BACKGROUND)</b></p>"
            "<p>LISTEN PORT   : <b>8080</b></p>"
            "<p>TOTAL HITS    : <b>ACTIVE CLIENTS SERVED</b></p>"
            "<p>HARDWARE CORE : <b>ESP32-C6 RISC-V</b></p>"
            "</div>"
            "<hr style='border:0;border-top:1px dashed #005533;margin:20px 0;'>"
            "<p style='font-size:12px;color:#558877;'>Managed concurrently by OpenC6 Preemptive Job Control.</p>"
            "</body></html>";

            abi->tcp_write(client_fd, http_response, str_len(http_response));
            abi->delay_ms(50);
            abi->tcp_close(client_fd);

            abi->set_led_color(0, 0, 0);

            abi->print(" -> [HTTP:8080] Hit #");
            print_uint32(abi, hit_count);
            abi->print(" served successfully!\r\n");
        }
    }
}
