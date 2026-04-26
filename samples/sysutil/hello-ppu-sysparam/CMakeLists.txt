# hello-ppu-sysparam — CMake build.
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build
# Produces:  build/hello-ppu-sysparam{,.stripped.elf,.self,.fake.self}

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-sysparam C)

include(ps3-self)

add_executable(hello-ppu-sysparam source/main.c)
target_compile_features(hello-ppu-sysparam PRIVATE c_std_11)

target_link_libraries(hello-ppu-sysparam PRIVATE
    sysutil rt lv2 
)

ps3_add_self(hello-ppu-sysparam)
