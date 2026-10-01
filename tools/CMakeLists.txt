cmake_minimum_required(VERSION 3.16)
project(OpenC6_Tools C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(ABI_INCLUDE_DIR "${PROJECT_ROOT}/components/openc6_abi/include")

# ==============================================================================
# 1. HOST UTILITIES (openc6_loader, zc6_pack)
# ==============================================================================

add_executable(openc6_loader openc6_loader.cpp)

add_subdirectory(zc6_pack)

# ==============================================================================
# 2. TARGET PAYLOADS (RISC-V Bare-metal)
# ==============================================================================

find_program(RISCV_GCC riscv32-esp-elf-gcc)
find_program(RISCV_OBJCOPY riscv32-esp-elf-objcopy)

if(RISCV_GCC AND RISCV_OBJCOPY)
    message(STATUS "Found ESP-IDF RISC-V Toolchain: ${RISCV_GCC}")
    message(STATUS "Using ABI Include Path: ${ABI_INCLUDE_DIR}")

    set(PAYLOAD_LD "${CMAKE_CURRENT_SOURCE_DIR}/payload.ld")
    set(PAYLOAD_OUT_DIR "${CMAKE_BINARY_DIR}/bin/payloads")
    file(MAKE_DIRECTORY ${PAYLOAD_OUT_DIR})

    # Auto-discover all example/*.c payloads
    file(GLOB PAYLOAD_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/example/*.c")
    set(ALL_PAYLOAD_BINS "")

    foreach(SRC_FILE ${PAYLOAD_SOURCES})
        get_filename_component(PAYLOAD_NAME ${SRC_FILE} NAME_WE)
        set(ELF_FILE "${CMAKE_CURRENT_BINARY_DIR}/${PAYLOAD_NAME}.elf")
        set(BIN_FILE "${PAYLOAD_OUT_DIR}/${PAYLOAD_NAME}.bin")

        add_custom_command(
            OUTPUT ${BIN_FILE}
            COMMAND ${RISCV_GCC} -march=rv32imac -mabi=ilp32 -Os -fPIC -fno-jump-tables -fno-tree-switch-conversion -nostdlib
                                 -I${ABI_INCLUDE_DIR}
                                 -Wl,-T,${PAYLOAD_LD} -Wl,--no-warn-rwx-segments
                                 -o ${ELF_FILE} ${SRC_FILE}
            COMMAND ${RISCV_OBJCOPY} -O binary ${ELF_FILE} ${BIN_FILE}
            DEPENDS ${SRC_FILE} ${PAYLOAD_LD}
            COMMENT "Compiling bare-metal payload: ${PAYLOAD_NAME}.bin -> build/bin/payloads/"
        )

        list(APPEND ALL_PAYLOAD_BINS ${BIN_FILE})
    endforeach()

    add_custom_target(payloads ALL DEPENDS ${ALL_PAYLOAD_BINS})

else()
    message(WARNING "ESP-IDF Toolchain NOT FOUND! Run '. $IDF_PATH/export.sh' first! Payloads will not be built.")
endif()
