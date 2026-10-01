add_executable(zc6_pack
    src/zc6_pack.c
    src/zc6_compress.c
    src/zc6_decompress.c
    src/hal_mem_posix.c
)

target_include_directories(zc6_pack PRIVATE include)
