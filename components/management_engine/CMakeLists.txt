idf_component_register(SRCS "management_engine.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_hw_support ulp log esp_system)
ulp_embed_binary("me_core" "ulp/lp_core_main.c" "management_engine.c")
