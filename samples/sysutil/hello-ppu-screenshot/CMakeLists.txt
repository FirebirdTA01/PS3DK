# hello-ppu-screenshot — CMake build.
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build
# Produces:  build/hello-ppu-screenshot{,.stripped.elf,.self,.fake.self}

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-screenshot C)

include(ps3-self)

add_executable(hello-ppu-screenshot source/main.c)
target_compile_features(hello-ppu-screenshot PRIVATE c_std_11)

target_link_libraries(hello-ppu-screenshot PRIVATE
    sysutil_screenshot_stub sysutil rt lv2 
)

ps3_add_self(hello-ppu-screenshot)
