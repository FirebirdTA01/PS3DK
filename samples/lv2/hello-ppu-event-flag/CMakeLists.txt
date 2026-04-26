# hello-ppu-event-flag — CMake build (mirrors Makefile; both coexist until phase 7e).
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build
# Produces:  build/hello-ppu-event-flag{,.stripped.elf,.self,.fake.self}

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-event-flag C)

include(ps3-self)

add_executable(hello-ppu-event-flag source/main.c)
target_compile_features(hello-ppu-event-flag PRIVATE c_std_11)

target_link_libraries(hello-ppu-event-flag PRIVATE
    rt lv2 
)

ps3_add_self(hello-ppu-event-flag)
