idf_component_register(SRCS "power_tweaker.c" "power_governor.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_pm esp_hw_support nvram esp_system freertos esp_timer heap management_engine esp_driver_tsens hal)
