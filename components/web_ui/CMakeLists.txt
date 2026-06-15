idf_component_register(SRCS "web_server.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_server nvram cjson spi_flash esp_partition nvs_flash esp_hw_support heap management_engine
                       EMBED_FILES "www/index.html")
