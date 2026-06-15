idf_component_register(SRCS "boot_menu.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_timer esp_partition freertos management_engine led_mgmt heap esp_driver_uart esp_driver_usb_serial_jtag mbedtls nvram wifi_mgmt)
