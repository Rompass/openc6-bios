#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_partition.h"
#include "driver/gpio.h"
#include "rom/ets_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "psa/crypto.h"
#include <math.h>
#include "esp_flash.h"

#include "bios_core.h"
#include "nvram.h"
#include "led_mgmt.h"
#include "wifi_mgmt.h"
#include "web_ui.h"
#include "boot_manager.h"
#include "power_tweaker.h"
#include "management_engine.h"
#include "me_shared.h"
#include "openc6_abi.h"
#include "pxe_boot.h"

// File system components inclusion
#include "openc6_fs.h"
#include "hal_flash.h"

#define BIOS_AP_SSID "BIOS_SETUP_C6"

// GPIO pins for power button (Software GND and Sense)
#define PIN_BTN_GND   3
#define PIN_BTN_SENSE 4
#define PIN_BTN_BOOT  9

static const char *TAG = "BIOS_CORE";

bool is_me_enabled = true;

static void bios_delay_ms(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static void* bios_malloc(uint32_t size) {
    return malloc((size_t)size);
}

// ─── CONSOLE OUTPUT WRAPPER (PRINT) ──────────────────────────────────────────────

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

// ─── FILE SYSTEM ABI WRAPPERS ──────────────────────────────────────────────────

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

    // Integer-only math extensions
    .math_isqrt = bios_math_isqrt,
    .math_sin_deg = bios_math_sin_deg,
    .math_cos_deg = bios_math_cos_deg,

    // Wi-Fi subsystem APIs
    .wifi_connect = bios_wifi_connect,
    .wifi_start_ap = bios_wifi_start_ap,
    .wifi_is_connected = bios_wifi_is_connected,

    // System telemetry and RAM sizing
    .get_free_ram = bios_get_free_ram,
    .get_total_ram = bios_get_total_ram,
    .get_total_flash = bios_get_total_flash,

    // File System ABI exports
    .fs_write_file = abi_fs_write_file,
    .fs_read_file = abi_fs_read_file,
    .fs_delete = abi_fs_delete
};

// ─── LP Core Software Watchdog feeder ────────────────────────────────────────
static void me_watchdog_pet_task(void *arg)
{
    for (;;) {
        management_engine_pet_watchdog();
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
// ─────────────────────────────────────────────────────────────────────────────

void bios_enter_s5_state(void)
{
    ESP_LOGW(TAG, "Entering S5 Soft-Off state...");
    led_mgmt_set_aura_mode(AURA_DISABLED);
    led_mgmt_set_color(0, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(100));

    if (is_me_enabled) {
        ESP_LOGI(TAG, "S5: Power Off. Management Engine running in background.");
        ulp_me_hp_is_awake = 0;
        esp_sleep_enable_ulp_wakeup();
    } else {
        ESP_LOGW(TAG, "S5: Legacy Mode. Using GPIO %d (Sense) and GPIO %d (GND) for wakeup.", PIN_BTN_SENSE, PIN_BTN_GND);

        gpio_reset_pin((gpio_num_t)PIN_BTN_GND);
        gpio_set_direction((gpio_num_t)PIN_BTN_GND, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)PIN_BTN_GND, 0);
        gpio_hold_en((gpio_num_t)PIN_BTN_GND);

        gpio_reset_pin((gpio_num_t)PIN_BTN_SENSE);
        gpio_set_direction((gpio_num_t)PIN_BTN_SENSE, GPIO_MODE_INPUT);
        gpio_set_pull_mode((gpio_num_t)PIN_BTN_SENSE, GPIO_PULLUP_ONLY);

        esp_sleep_enable_ext1_wakeup(1ULL << PIN_BTN_SENSE, ESP_EXT1_WAKEUP_ANY_LOW);
    }

    esp_deep_sleep_start();
}

void bios_core_start(void)
{
    esp_reset_reason_t rst_reason = esp_reset_reason();
    bool is_cold_boot = (rst_reason != ESP_RST_DEEPSLEEP);
    uint32_t legacy_hold_ms = 0;

    if (!is_cold_boot && (esp_sleep_get_wakeup_causes() & (1ULL << ESP_SLEEP_WAKEUP_EXT1))) {
        gpio_reset_pin((gpio_num_t)PIN_BTN_GND);
        gpio_set_direction((gpio_num_t)PIN_BTN_GND, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)PIN_BTN_GND, 0);

        gpio_reset_pin((gpio_num_t)PIN_BTN_SENSE);
        gpio_set_direction((gpio_num_t)PIN_BTN_SENSE, GPIO_MODE_INPUT);
        gpio_set_pull_mode((gpio_num_t)PIN_BTN_SENSE, GPIO_PULLUP_ONLY);

        ESP_LOGI(TAG, "Legacy Wakeup: Measuring button hold duration...");
        while (gpio_get_level((gpio_num_t)PIN_BTN_SENSE) == 0 && legacy_hold_ms < 3000) {
            vTaskDelay(pdMS_TO_TICKS(10));
            legacy_hold_ms += 10;
        }
    }

    gpio_hold_dis((gpio_num_t)PIN_BTN_GND);

    vTaskDelay(pdMS_TO_TICKS(4500));
    ESP_LOGI(TAG, "--- OpenC6 BIOS v1.1-ME Initializing ---");
    ESP_LOGI(TAG, "Reset reason: %d, cold_boot: %s", rst_reason, is_cold_boot ? "YES" : "NO");

    uint32_t me_reason = management_engine_get_boot_reason();
    management_engine_clear_boot_reason();
    ESP_LOGI(TAG, "ME boot reason: %lu", me_reason);

    nvram_init();
    led_mgmt_init();
    wifi_mgmt_init();

    hal_flash_init();
    fs_init();

    gpio_reset_pin((gpio_num_t)PIN_BTN_BOOT);
    gpio_set_direction((gpio_num_t)PIN_BTN_BOOT, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)PIN_BTN_BOOT, GPIO_PULLUP_ONLY);

    me_state_t me_state;
    nvram_get_me_state(&me_state);
    is_me_enabled = (me_state == ME_ENABLED);
    ESP_LOGI(TAG, "Management Engine is %s", is_me_enabled ? "ENABLED" : "DISABLED");

    management_engine_init(is_me_enabled, is_cold_boot);

    if (is_me_enabled) {
        ulp_me_hp_is_awake = 1;
        xTaskCreate(me_watchdog_pet_task, "me_wdt", 2048, NULL, 5, NULL);
    }

    // =========================================================================
    // 🔴 NETWORK BIOS UPDATE SERVICE (PXE-BASED AUTO OTA) 🔴
    // =========================================================================
    bios_update_state_t ota_state;
    nvram_get_bios_update_state(&ota_state);

    if (ota_state == BIOS_UPDATE_PENDING) {
        ESP_LOGW(TAG, ">>> NETWORK BIOS UPDATE PENDING <<<");

        nvram_set_bios_update_state(BIOS_UPDATE_NONE);

        led_mgmt_set_aura_mode(AURA_DISABLED);
        led_mgmt_set_color(0, 0, 255);

        if (wifi_mgmt_start_sta() == ESP_OK) {
            char pxe_url[PXE_URL_MAX_LEN + 1] = {0};
            nvram_get_pxe_url(pxe_url, sizeof(pxe_url));

            if (pxe_bios_ota_execute(pxe_url)) {
                ESP_LOGI(TAG, "Update Complete! Rebooting into NEW BIOS...");
                led_mgmt_set_color(0, 255, 0);
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_restart();
            } else {
                ESP_LOGE(TAG, "Update Failed! Continuing normal boot...");
                led_mgmt_set_color(255, 0, 0);
                vTaskDelay(pdMS_TO_TICKS(2000));
            }
        } else {
            ESP_LOGE(TAG, "WiFi connection failed. Cannot update BIOS. Continuing normal boot...");
            led_mgmt_set_color(255, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
    // =========================================================================

    gpio_reset_pin((gpio_num_t)PIN_CMOS_GND);
    gpio_set_direction((gpio_num_t)PIN_CMOS_GND, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)PIN_CMOS_GND, 0);

    gpio_reset_pin((gpio_num_t)PIN_CLEAR_NVRAM);
    gpio_set_direction((gpio_num_t)PIN_CLEAR_NVRAM, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)PIN_CLEAR_NVRAM, GPIO_PULLUP_ONLY);

    vTaskDelay(pdMS_TO_TICKS(10));

    if (gpio_get_level((gpio_num_t)PIN_CLEAR_NVRAM) == 0) {
        led_mgmt_set_aura_mode(AURA_DISABLED);
        led_mgmt_set_color(COLOR_POST_ERROR);
        ESP_LOGE(TAG, "Jumper on PIN %d detected! Clearing NVRAM...", PIN_CLEAR_NVRAM);
        nvram_load_defaults();
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }
    gpio_set_direction((gpio_num_t)PIN_CMOS_GND, GPIO_MODE_DISABLE);

    power_tweaker_apply_bios_settings();

    ESP_LOGI(TAG, "Executing POST...");
    led_mgmt_blink_post(COLOR_POST_OK, 3);

    esp_ota_img_states_t ota_img_state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &ota_img_state) == ESP_OK) {
        if (ota_img_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "POST OK! Confirming stable boot to Rollback Manager...");
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }

    bool should_boot = false;
    bool enter_setup = false;
    bool serial_boot_ram = false;
    bool serial_boot_flash = false;
    bool direct_flash_boot = false;

    // =========================================================================
    // --- INTERACTIVE BOOT MENU DIAGNOSTIC (GPIO 9) ---
    // =========================================================================
    if (gpio_get_level((gpio_num_t)PIN_BTN_BOOT) == 0) {
        uint32_t hold_time = 0;
        while (gpio_get_level((gpio_num_t)PIN_BTN_BOOT) == 0 && hold_time < 1000) {
            vTaskDelay(pdMS_TO_TICKS(10));
            hold_time += 10;
        }

        if (hold_time >= 1000) {
            boot_option_t user_choice = boot_manager_interactive_menu();

            if (user_choice == BOOT_OPT_NETWORK) should_boot = true;
            else if (user_choice == BOOT_OPT_SERIAL_RAM) serial_boot_ram = true;
            else if (user_choice == BOOT_OPT_SERIAL_FLASH) serial_boot_flash = true;
            else if (user_choice == BOOT_OPT_DEFAULT) direct_flash_boot = true;
            else if (user_choice == BOOT_OPT_SETUP) enter_setup = true;

            goto execute_boot;
        }
    }

    // =========================================================================
    // --- BOOT REASON ANALYSIS MATRIX (DVFS & SECURITY) ---
    // =========================================================================
    if (me_reason == ME_BOOT_REASON_THERMAL) {
        uint8_t th, em;
        nvram_get_thermal_limits(&th, &em);
        ESP_LOGE(TAG, "THERMAL EMERGENCY! LP Core detected temperature >= %d°C. Powering off.", em);
        bios_enter_s5_state();
    }
    else if (me_reason == ME_BOOT_REASON_FORCE_RESET) {
        ESP_LOGE(TAG, "ME Hard Reset detected (5s+ hold). Powering off.");
        bios_enter_s5_state();
    }
    else if (me_reason == ME_BOOT_REASON_WDT) {
        ESP_LOGE(TAG, "ME Watchdog Reset! HP Core was unresponsive. Recovery boot...");
        direct_flash_boot = true;
    }
    else if (me_reason == ME_BOOT_REASON_SETUP || legacy_hold_ms >= 3000) {
        ESP_LOGI(TAG, "Wakeup: BIOS Setup requested.");
        enter_setup = true;
    }
    else if (me_reason == ME_BOOT_REASON_NORMAL || legacy_hold_ms > 0) {
        ESP_LOGI(TAG, "Wakeup: Normal Boot requested. Checking File System...");
        direct_flash_boot = true;
    }
    else if (is_cold_boot) {
        dc_loss_action_t dc_action;
        nvram_get_dc_loss_action(&dc_action);
        if (dc_action == DC_LOSS_POWER_OFF) {
            ESP_LOGI(TAG, "DC Loss Action: POWER_OFF. Going to S5.");
            bios_enter_s5_state();
        } else {
            ESP_LOGI(TAG, "DC Loss Action: BOOT. Starting normally. Checking File System...");
            direct_flash_boot = true;
        }
    }

    execute_boot:
    // =========================================================================
    // --- STATE MACHINE ROUTER & EXECUTION ---
    // =========================================================================
    aura_mode_t aura;
    nvram_get_aura_mode(&aura);

    // A) SERIAL BOOT TO VOLATILE SRAM
    if (serial_boot_ram) {
        serial_boot_ram = false;
        ESP_LOGW(TAG, ">>> ENTERING SERIAL BOOT MODE (RAM target) <<<");
        led_mgmt_set_aura_mode(AURA_DISABLED);
        led_mgmt_set_color(255, 0, 255);

        if (!boot_manager_serial_listen(30, PAYLOAD_TARGET_RAM)) {
            ESP_LOGE(TAG, "Serial RAM Boot failed/timeout! Falling back to Setup...");
            enter_setup = true;
            goto execute_boot;
        }
    }

    // B) SERIAL BOOT TO NON-VOLATILE FLASH(UNIX SHELL)
    if (serial_boot_flash) {
        serial_boot_flash = false;
        boot_manager_shell();
        goto execute_boot;
    }

    // C) PXE WIRELESS NETWORK BOOT
    if (should_boot) {
        should_boot = false;
        if (aura == AURA_RAINBOW) led_mgmt_set_aura_mode(AURA_RAINBOW);
        else led_mgmt_set_color(COLOR_POST_OK);

        ESP_LOGI(TAG, "Attempting to connect to Network (PXE Boot)...");
        if (wifi_mgmt_start_sta() == ESP_OK) {
            ESP_LOGI(TAG, "Network Ready. Handing over to Boot Manager (PXE)...");

            char pxe_url[PXE_URL_MAX_LEN + 1] = {0};
            nvram_get_pxe_url(pxe_url, sizeof(pxe_url));

            if (pxe_boot_execute(pxe_url)) {
                ESP_LOGI(TAG, "PXE Success! Redirecting to Flash Boot...");
                direct_flash_boot = true;
                goto execute_boot;
            } else {
                ESP_LOGE(TAG, "PXE Boot failed! Falling back to Serial Boot...");
                serial_boot_ram = true;
                goto execute_boot;
            }
        } else {
            ESP_LOGE(TAG, "Network failed! Falling back to Serial Boot...");
            serial_boot_ram = true;
            goto execute_boot;
        }
    }

    // D) EXECUTE-IN-PLACE (XIP) STABLE LOCAL BOOT FROM FILE SYSTEM (downloaded/payload.bin)
    if (direct_flash_boot) {
        direct_flash_boot = false;
        ESP_LOGI(TAG, "Checking for Stored OS (payload.bin) in OpenC6 File System...");

        const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0xff, "openc6_fs");
        if (part) {
            uint16_t boot_dir_id = 0;

            int16_t dir_id = fs_find_id("downloaded", 0);
            if (dir_id >= 0 && fs_get_type(dir_id) == TYPE_DIR) {
                boot_dir_id = (uint16_t)dir_id;
            }

            int16_t file_id = fs_find_id("payload.bin", boot_dir_id);
            if (file_id >= 0 && fs_get_type(file_id) == TYPE_FILE) {
                int32_t file_size = fs_get_size(file_id);
                if (file_size > 0) {
                    led_mgmt_set_aura_mode(AURA_DISABLED);
                    led_mgmt_set_color(0, 255, 0);

                    void *payload_ram_ptr = heap_caps_malloc(file_size, MALLOC_CAP_EXEC | MALLOC_CAP_INTERNAL);
                    if (payload_ram_ptr != NULL) {
                        ESP_LOGW(TAG, ">>> LAUNCHING STORED OS (payload.bin) FROM FS TO RAM <<<");
                        int32_t r_bytes = fs_read_file(file_id, payload_ram_ptr, 0, file_size);
                        if (r_bytes == file_size) {
                            ESP_LOGI(TAG, "Payload loaded to RAM at %p. Jumping...", payload_ram_ptr);
                            vTaskDelay(pdMS_TO_TICKS(100));

                            typedef void (*payload_entry_t)(const openc6_abi_t *) __attribute__((noreturn));
                            payload_entry_t launch_payload = (payload_entry_t)payload_ram_ptr;

                            asm volatile ("fence.i");
                            launch_payload(&bios_abi);
                        } else {
                            ESP_LOGE(TAG, "Failed to read payload from FS!");
                            free(payload_ram_ptr);
                            enter_setup = true;
                            goto execute_boot;
                        }
                    } else {
                        ESP_LOGW(TAG, ">>> RAM FULL. LAUNCHING STORED OS VIA XIP FALLBACK <<<");
                        const esp_partition_t *xip_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x20, "payload_xip");
                        if (xip_part && file_size <= xip_part->size) {
                            ESP_LOGI(TAG, "Deploying %ld bytes to XIP partition...", file_size);
                            esp_partition_erase_range(xip_part, 0, (file_size + 4095) & ~4095);

                            uint8_t buf[1024];
                            uint32_t offset = 0;
                            bool copy_ok = true;
                            while (offset < file_size) {
                                uint32_t chunk = (file_size - offset > sizeof(buf)) ? sizeof(buf) : (file_size - offset);
                                if (fs_read_file(file_id, buf, offset, chunk) != chunk) {
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
                                    ESP_LOGI(TAG, "XIP mapped at %p. Jumping...", mapped_ptr);
                                    vTaskDelay(pdMS_TO_TICKS(100));

                                    typedef void (*payload_entry_t)(const openc6_abi_t *) __attribute__((noreturn));
                                    payload_entry_t launch_payload = (payload_entry_t)mapped_ptr;

                                    asm volatile ("fence.i");
                                    launch_payload(&bios_abi);
                                } else {
                                    ESP_LOGE(TAG, "Failed to map XIP partition!");
                                }
                            } else {
                                ESP_LOGE(TAG, "Failed to copy payload to XIP partition!");
                            }
                        } else {
                            ESP_LOGE(TAG, "File too large for XIP partition or partition missing!");
                        }
                        enter_setup = true;
                        goto execute_boot;
                    }
                } else {
                    ESP_LOGE(TAG, "payload.bin is empty! Opening Boot Menu...");
                    boot_option_t user_choice = boot_manager_interactive_menu();

                    if (user_choice == BOOT_OPT_NETWORK) should_boot = true;
                    else if (user_choice == BOOT_OPT_SERIAL_RAM) serial_boot_ram = true;
                    else if (user_choice == BOOT_OPT_SERIAL_FLASH) serial_boot_flash = true;
                    else if (user_choice == BOOT_OPT_DEFAULT) direct_flash_boot = true;
                    else if (user_choice == BOOT_OPT_SETUP) enter_setup = true;

                    goto execute_boot;
                }
            } else {
                ESP_LOGE(TAG, "payload.bin not found inside '/downloaded/'! Opening Boot Menu...");
                boot_option_t user_choice = boot_manager_interactive_menu();

                if (user_choice == BOOT_OPT_NETWORK) should_boot = true;
                else if (user_choice == BOOT_OPT_SERIAL_RAM) serial_boot_ram = true;
                else if (user_choice == BOOT_OPT_SERIAL_FLASH) serial_boot_flash = true;
                else if (user_choice == BOOT_OPT_DEFAULT) direct_flash_boot = true;
                else if (user_choice == BOOT_OPT_SETUP) enter_setup = true;

                goto execute_boot;
            }
        } else {
            ESP_LOGE(TAG, "Partition 'openc6_fs' is missing!");
            enter_setup = true;
            goto execute_boot;
        }
    }

    // E) BIOS SETUP UTILITY (LOCAL AP WEB CONFIGURATOR)
    if (enter_setup) {
        enter_setup = false;
        ESP_LOGW(TAG, ">>> ENTERING BIOS SETUP (WEB UI) <<<");
        if (aura == AURA_RAINBOW) led_mgmt_set_aura_mode(AURA_RAINBOW);
        else led_mgmt_set_color(COLOR_BIOS_SETUP);
        wifi_mgmt_start_ap(BIOS_AP_SSID, "12345678");
        web_ui_start();
        ESP_LOGI(TAG, "AP '%s' is active. Connect to 192.168.4.1", BIOS_AP_SSID);

        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    // F) Power State Fallback
    if (!serial_boot_ram && !serial_boot_flash && !direct_flash_boot && !should_boot && !enter_setup) {
        ESP_LOGW(TAG, "No boot action determined. Going to S5.");
        bios_enter_s5_state();
    }

    while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}
