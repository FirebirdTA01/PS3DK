# hello-ppu-subdisplay - CMake build.
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build
# Produces:  build/hello-ppu-subdisplay{,.stripped.elf,.self,.fake.self}

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-subdisplay C)

include(ps3-self)

add_executable(hello-ppu-subdisplay source/main.c)
target_compile_features(hello-ppu-subdisplay PRIVATE c_std_11)

target_link_libraries(hello-ppu-subdisplay PRIVATE
    sysutil_subdisplay_stub sysutil rt lv2
)

ps3_add_self(hello-ppu-subdisplay)
