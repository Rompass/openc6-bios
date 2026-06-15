idf_component_register(SRCS "nvram.c" "nvram_defaults.c"
                       INCLUDE_DIRS "include"
                       REQUIRES nvs_flash esp_system)
