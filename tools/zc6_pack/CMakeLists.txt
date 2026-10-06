set(ZC6_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../components/zc6)

add_executable(zc6_pack
    src/zc6_pack.c
    src/hal_mem_posix.c
    ${ZC6_DIR}/zc6_compress.c
    ${ZC6_DIR}/zc6_decompress.c
)

target_include_directories(zc6_pack PRIVATE ${ZC6_DIR}/include)
target_compile_definitions(zc6_pack PRIVATE ZC6_TELEMETRY=1)
