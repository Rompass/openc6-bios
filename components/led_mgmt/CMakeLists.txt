idf_component_register(SRCS "led_mgmt.c"
                       INCLUDE_DIRS "include"
                       REQUIRES led_strip nvram bios_core freertos)
