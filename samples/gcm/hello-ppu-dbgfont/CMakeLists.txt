# hello-ppu-dbgfont — CMake build (mirrors Makefile; both coexist until phase 7e).
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build
# Produces:  build/hello-ppu-dbgfont{,.stripped.elf,.self,.fake.self}

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-dbgfont C)

include(ps3-self)

add_executable(hello-ppu-dbgfont source/main.c)
target_compile_features(hello-ppu-dbgfont PRIVATE c_std_11)

target_link_libraries(hello-ppu-dbgfont PRIVATE
    dbgfont gcm_cmd gcm_sys io sysutil rt lv2 m 
)

ps3_add_self(hello-ppu-dbgfont)
