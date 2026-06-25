idf_component_register(SRCS "openc6_fs.c" "hal_flash.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES spi_flash esp_partition)
