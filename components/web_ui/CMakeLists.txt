idf_component_register(SRCS "web_server.c" "c6wsh.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_server nvram cjson spi_flash esp_partition nvs_flash
                                esp_hw_support heap management_engine openc6_abi openc6_fs
                                sandbox wifi_mgmt pxe_boot led_mgmt boot_manager
                       EMBED_FILES "www/index.html")
