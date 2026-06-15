cmake_minimum_required(VERSION 3.16)
project(OpenC6_Tools)

# Common settings for C++
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Determine the repository root directory (one level up from the tools/ directory)
get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# Automatically define the independent path to the ABI headers
set(ABI_INCLUDE_DIR "${PROJECT_ROOT}/components/boot_manager/include")

# ==============================================================================
# 1. HOST UTILITY COMPILATION (openc6_loader for PC)
# ==============================================================================
add_executable(openc6_loader openc6_loader.cpp)

# Post-build command to copy the compiled loader to the root of the tools/ directory
add_custom_command(TARGET openc6_loader POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:openc6_loader> ${CMAKE_CURRENT_SOURCE_DIR}/
    COMMENT "Copied openc6_loader to tools/ directory."
)

# ==============================================================================
# 2. TARGET PAYLOAD COMPILATION (for ESP32-C6 RISC-V)
# ==============================================================================

# Search for the ESP-IDF cross-compiler in environment variables (export.sh must be activated)
find_program(RISCV_GCC riscv32-esp-elf-gcc)
find_program(RISCV_OBJCOPY riscv32-esp-elf-objcopy)

if(RISCV_GCC AND RISCV_OBJCOPY)
    message(STATUS "Found ESP-IDF RISC-V Toolchain: ${RISCV_GCC}")
    message(STATUS "Using ABI Include Path: ${ABI_INCLUDE_DIR}")

    set(PAYLOAD_SRC "${CMAKE_CURRENT_SOURCE_DIR}/example/payload.c")
    set(PAYLOAD_LD  "${CMAKE_CURRENT_SOURCE_DIR}/payload.ld")
    set(PAYLOAD_ELF "${CMAKE_CURRENT_BINARY_DIR}/payload.elf")
    set(PAYLOAD_BIN "${CMAKE_CURRENT_SOURCE_DIR}/payload.bin") # Places payload.bin directly into tools/ root

    # Custom command calling the ESP compiler with specific bare-metal flags and ABI include path
    add_custom_command(
        OUTPUT ${PAYLOAD_BIN}
        COMMAND ${RISCV_GCC} -march=rv32imac -mabi=ilp32 -Os -fPIC -fno-jump-tables -fno-tree-switch-conversion -nostdlib
                             -I${ABI_INCLUDE_DIR}
                             -Wl,-T,${PAYLOAD_LD} -Wl,--no-warn-rwx-segments
                             -o ${PAYLOAD_ELF} ${PAYLOAD_SRC}
        COMMAND ${RISCV_OBJCOPY} -O binary ${PAYLOAD_ELF} ${PAYLOAD_BIN}
        DEPENDS ${PAYLOAD_SRC} ${PAYLOAD_LD}
        COMMENT "Compiling and building bare-metal Payload for OpenC6 BIOS..."
    )

    # Define target so CMake knows to build the payload
    add_custom_target(payload ALL DEPENDS ${PAYLOAD_BIN})

else()
    message(WARNING "ESP-IDF Toolchain NOT FOUND! Run '. $IDF_PATH/export.sh' first! Payload will not be built.")
endif()
