# hello-ppu-savedata-list - CMake build.
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build
# Produces:  build/hello-ppu-savedata-list{,.stripped.elf,.self,.fake.self}

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-savedata-list C)

include(ps3-self)

add_executable(hello-ppu-savedata-list source/main.c)
target_compile_features(hello-ppu-savedata-list PRIVATE c_std_11)

target_link_libraries(hello-ppu-savedata-list PRIVATE
    sysutil_savedata_stub sysutil_savedata_extra_stub
    sysmodule_stub io sysutil rt lv2
)

ps3_add_self(hello-ppu-savedata-list)
