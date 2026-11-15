idf_build_get_property(target IDF_TARGET)

if(target STREQUAL "esp32")
    set(HAL_TARGET "esp32")
else()
    string(REGEX REPLACE "^esp32" "" HAL_TARGET "${target}")
endif()

get_filename_component(HAL_TARGET_INC_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../open_hal/targets/${HAL_TARGET}/include"
    ABSOLUTE
)

set(HAS_LP_CORE FALSE)
if(EXISTS "${HAL_TARGET_INC_DIR}/hal_board.h")
    file(READ "${HAL_TARGET_INC_DIR}/hal_board.h" _hal_board_hdr)
    if(_hal_board_hdr MATCHES "#define[ \t]+HAL_CAP_LP_CORE[ \t]+1")
        set(HAS_LP_CORE TRUE)
    endif()
endif()

if(HAS_LP_CORE)
    idf_component_register(
        SRCS "management_engine.c"
        INCLUDE_DIRS "include" "${HAL_TARGET_INC_DIR}"
        REQUIRES esp_hw_support ulp log esp_system open_hal
    )
    ulp_embed_binary("me_core" "ulp/lp_core_main.c" "management_engine.c")
else()
    idf_component_register(
        SRCS "management_engine.c"
        INCLUDE_DIRS "include"
        REQUIRES esp_hw_support log esp_system open_hal
    )
endif()
