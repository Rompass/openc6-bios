idf_component_register(SRCS "led_mgmt.c"
                       INCLUDE_DIRS "include"
                       REQUIRES nvram bios_core freertos esp_hal_rmt)
