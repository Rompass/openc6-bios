idf_component_register(SRCS "bios_init.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_gpio esp_hw_support esp_system nvram led_mgmt wifi_mgmt web_ui boot_manager power_mgmt management_engine pxe_boot openc6_fs sandbox)

target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=esp_cpu_configure_region_protection"
    "-Wl,--wrap=esp_restart_noos"
)
