# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_c6_bios)

# Auto-select partition table based on target
if(CONFIG_IDF_TARGET_ESP32C3)
    # C3 SuperMini with 4 MB flash
    if(NOT DEFINED PARTITION_TABLE_CUSTOM_CSV_PATH)
        set(PARTITION_TABLE_CUSTOM_CSV_PATH "${CMAKE_SOURCE_DIR}/partitions_c3_4mb.csv")
    endif()
    message(STATUS "Building for ESP32-C3 SuperMini (4 MB flash)")
else()
    # Default C6 with 8 MB flash
    message(STATUS "Building for ESP32-C6 (8 MB flash)")
endif()
