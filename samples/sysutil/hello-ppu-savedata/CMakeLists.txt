# hello-ppu-savedata — CMake build.
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build
# Produces:  build/hello-ppu-savedata{,.stripped.elf,.self,.fake.self}

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-savedata C)

include(ps3-self)

add_executable(hello-ppu-savedata source/main.c)
target_compile_features(hello-ppu-savedata PRIVATE c_std_11)

target_link_libraries(hello-ppu-savedata PRIVATE
    sysutil rt lv2 
)

ps3_add_self(hello-ppu-savedata)
