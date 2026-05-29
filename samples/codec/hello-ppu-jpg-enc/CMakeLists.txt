# hello-ppu-jpg-enc - CMake build.
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build
# Produces:  build/hello-ppu-jpg-enc{,.stripped.elf,.self,.fake.self}

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-jpg-enc C)

include(ps3-self)

add_executable(hello-ppu-jpg-enc source/main.c)
target_compile_features(hello-ppu-jpg-enc PRIVATE c_std_11)

target_link_libraries(hello-ppu-jpg-enc PRIVATE
    jpgenc_stub sysmodule_stub rt lv2 m
)

ps3_add_self(hello-ppu-jpg-enc)
