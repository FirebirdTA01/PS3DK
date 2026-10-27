# hello-ppu-lv2-memcontainer - CMake build.
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build
# Produces:  build/hello-ppu-lv2-memcontainer{,.stripped.elf,.self,.fake.self}

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-lv2-memcontainer C)

include(ps3-self)

add_executable(hello-ppu-lv2-memcontainer source/main.c)
target_compile_features(hello-ppu-lv2-memcontainer PRIVATE c_std_11)

target_link_libraries(hello-ppu-lv2-memcontainer PRIVATE rt lv2)

ps3_add_self(hello-ppu-lv2-memcontainer)
