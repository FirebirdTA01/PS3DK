# hello-ppu-gamedata — CMake build (mirrors Makefile; both coexist until phase 7e).
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build
# Produces:  build/hello-ppu-gamedata{,.stripped.elf,.self,.fake.self}

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-gamedata C)

include(ps3-self)

add_executable(hello-ppu-gamedata source/main.c)
target_compile_features(hello-ppu-gamedata PRIVATE c_std_11)

target_link_libraries(hello-ppu-gamedata PRIVATE
    sysutil rt lv2 
)

ps3_add_self(hello-ppu-gamedata)
