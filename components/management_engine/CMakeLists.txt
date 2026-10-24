idf_component_register(SRCS "management_engine.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_hw_support ulp log esp_system)

# ESP32-C3 does not have LP-Core, so skip ULP binary embedding
if(NOT CONFIG_IDF_TARGET_ESP32C3)
    ulp_embed_binary("me_core" "ulp/lp_core_main.c" "management_engine.c")
endif()
