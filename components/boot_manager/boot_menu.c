#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "driver/gpio.h"

#include "boot_manager.h"
#include "hw_usb.h"
#include "sandbox.h"
#include "led_mgmt.h"

#define PIN_BTN_BOOT            9

#define CMD_ACK                 0x06
#define CMD_EOT                 0x04
#define CMD_NAK                 0x15

static const char *TAG = "BOOT_MGR";

static void blink_menu(boot_option_t opt)
{
    ESP_LOGI(TAG, "Option: [%d] %s", opt,
             opt == BOOT_OPT_DEFAULT ? "Default OS (Flash)" :
             opt == BOOT_OPT_SHELL   ? "Micro UNIX Shell"   : "BIOS Setup");

    led_mgmt_set_aura_mode(AURA_DISABLED);
    led_mgmt_blink_post(0, 255, 255, (uint32_t)opt + 1);
}

boot_option_t boot_manager_interactive_menu(void)
{
    gpio_reset_pin((gpio_num_t)PIN_BTN_BOOT);
    gpio_set_direction((gpio_num_t)PIN_BTN_BOOT, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)PIN_BTN_BOOT, GPIO_PULLUP_ONLY);

    boot_option_t current_opt = BOOT_OPT_DEFAULT;
    blink_menu(current_opt);

    while (1) {
        if (gpio_get_level((gpio_num_t)PIN_BTN_BOOT) == 0) {
            uint32_t press_duration_ms = 0;
            while (gpio_get_level((gpio_num_t)PIN_BTN_BOOT) == 0) {
                vTaskDelay(pdMS_TO_TICKS(10));
                press_duration_ms += 10;
            }

            /* Long press selects option, short tap cycles */
            if (press_duration_ms >= 1000) {
                led_mgmt_set_color(0, 255, 0);
                vTaskDelay(pdMS_TO_TICKS(500));
                return current_opt;
            } else if (press_duration_ms >= 50) {
                current_opt = (boot_option_t)(((uint32_t)current_opt + 1) % BOOT_OPT_MAX);
                blink_menu(current_opt);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

bool boot_manager_serial_listen(int timeout_sec, payload_target_t target)
{
    if (target == PAYLOAD_TARGET_FLASH) {
        ESP_LOGE(TAG, "Direct Flash serial streaming is disabled.");
        return false;
    }

    sandbox_init_dynamic(sandbox_get_abi(), SANDBOX_DEFAULT_KERNEL_RESERVE);
    size_t max_size = sandbox_get_arena_size();
    if (max_size == 0) {
        ESP_LOGE(TAG, "Sandbox arena allocation failed.");
        return false;
    }

    /* Drain any stale RX characters from USB FIFO */
    while (hw_usb_has_data()) {
        hw_usb_getc();
    }

    led_mgmt_set_color(255, 0, 255);

    uint8_t size_buf[4] = {0};
    uint32_t file_size = 0;
    uint64_t start_time = esp_timer_get_time();
    uint64_t last_sync_time = 0;
    uint64_t timeout_us = (uint64_t)timeout_sec * 1000000ULL;
    const char *fail_reason = "Unknown Error";

    int state = 0;

    while ((esp_timer_get_time() - start_time) < timeout_us) {
        uint64_t now = esp_timer_get_time();

        /* Broadcast synchronization beacon over native USB every 500ms */
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
        fail_reason = "Preamble timeout";
        goto error_exit;
    }

    file_size = (uint32_t)size_buf[0] |
    ((uint32_t)size_buf[1] << 8) |
    ((uint32_t)size_buf[2] << 16) |
    ((uint32_t)size_buf[3] << 24);

    if (file_size == 0 || (size_t)file_size > max_size) {
        hw_usb_putc(CMD_NAK);
        hw_usb_flush();
        fail_reason = "Payload exceeds dynamic Sandbox arena size";
        goto error_exit;
    }

    hw_usb_putc(CMD_ACK);
    hw_usb_flush();

    uint8_t *arena = sandbox_get_arena();
    uint32_t total_received = 0;
    int chunk_accum = 0;
    uint64_t last_data_time = esp_timer_get_time();

    /* Stream incoming payload chunks directly into sandbox arena registers */
    while (total_received < file_size) {
        if (hw_usb_has_data()) {
            arena[total_received++] = (uint8_t)hw_usb_getc();
            chunk_accum++;
            last_data_time = esp_timer_get_time();

            if (chunk_accum >= 64 || total_received == file_size) {
                hw_usb_putc(CMD_ACK);
                hw_usb_flush();
                chunk_accum = 0;
            }
        }

        if ((esp_timer_get_time() - last_data_time) > 5000000ULL) {
            fail_reason = "Stream transfer timeout";
            goto error_exit;
        }

        if (!hw_usb_has_data()) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    uint64_t eot_timeout = esp_timer_get_time();
    while ((esp_timer_get_time() - eot_timeout) < 1000000ULL) {
        if (hw_usb_has_data()) {
            if ((uint8_t)hw_usb_getc() == CMD_EOT) break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    ESP_LOGI(TAG, "USB payload received: %lu bytes. Entering Sandbox...", (unsigned long)total_received);
    led_mgmt_set_color(0, 255, 0);
    vTaskDelay(pdMS_TO_TICKS(100));

    sandbox_load_payload(arena, (size_t)file_size);
    sandbox_status_t status = sandbox_run();
    ESP_LOGW(TAG, "Serial payload session completed: status=%d", status);

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    error_exit:
    ESP_LOGE(TAG, "Serial Boot Failed: %s", fail_reason);
    return false;
}
