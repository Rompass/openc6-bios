idf_component_register(SRCS "bios_init.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_gpio esp_hw_support esp_system nvram led_mgmt wifi_mgmt web_ui boot_manager power_mgmt management_engine pxe_boot mbedtls)
