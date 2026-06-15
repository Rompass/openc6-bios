idf_component_register(SRCS "pxe_boot.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_client esp_partition log app_update)
