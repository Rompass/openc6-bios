idf_build_get_property(target IDF_TARGET)

if(target STREQUAL "esp32")
    set(HAL_TARGET "esp32")
else()
    string(REGEX REPLACE "^esp32" "" HAL_TARGET "${target}")
endif()

if(__ULP_BUILDV2)
    idf_component_register(
        INCLUDE_DIRS
            "include"
            "targets/${HAL_TARGET}/include"
    )
    return()
endif()

set(HAL_TARGET_DIR "${CMAKE_CURRENT_LIST_DIR}/targets/${HAL_TARGET}")
if(NOT EXISTS "${HAL_TARGET_DIR}/target.cmake")
    message(FATAL_ERROR "open_hal: Missing target definition '${HAL_TARGET_DIR}/target.cmake'")
endif()

include("${HAL_TARGET_DIR}/target.cmake")

idf_component_register(
    SRCS
        "hal_flash.c"
        ${HAL_TARGET_SRCS}
    INCLUDE_DIRS
        "include"
        "targets/${HAL_TARGET}/include"
    REQUIRES
        esp_partition
        ${HAL_TARGET_REQUIRES}
    PRIV_REQUIRES
        spi_flash
        ${HAL_TARGET_PRIV_REQUIRES}
)
