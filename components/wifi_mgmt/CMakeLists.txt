idf_component_register(SRCS "wifi_mgmt.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_wifi esp_netif esp_event nvram freertos)
