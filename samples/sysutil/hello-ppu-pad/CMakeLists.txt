# hello-ppu-pad — CMake build (mirrors Makefile; both coexist until phase 7e).
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build
# Produces:  build/hello-ppu-pad{,.stripped.elf,.self,.fake.self}

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-pad C)

include(ps3-self)

add_executable(hello-ppu-pad source/main.c)
target_compile_features(hello-ppu-pad PRIVATE c_std_11)

target_link_libraries(hello-ppu-pad PRIVATE
    io sysutil rt lv2 
)

ps3_add_self(hello-ppu-pad)
