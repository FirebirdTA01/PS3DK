# hello-ppu-input-combined — CMake build.
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build
# Produces:  build/hello-ppu-input-combined{,.stripped.elf,.self,.fake.self}

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-input-combined C)

include(ps3-self)

add_executable(hello-ppu-input-combined source/main.c)
target_compile_features(hello-ppu-input-combined PRIVATE c_std_11)

target_link_libraries(hello-ppu-input-combined PRIVATE
    io sysutil rt lv2
)

ps3_add_self(hello-ppu-input-combined)

ps3_add_pkg(hello-ppu-input-combined
    APPID     "SYUT00039"
    CONTENTID "UP0001-SYUT00039_00-PS3DKSAMPLES0000")
