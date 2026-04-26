# hello-ppu-audio-out — CMake build.
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build
# Produces:  build/hello-ppu-audio-out{,.stripped.elf,.self,.fake.self}

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-audio-out C)

include(ps3-self)

add_executable(hello-ppu-audio-out source/main.c)
target_compile_features(hello-ppu-audio-out PRIVATE c_std_11)

target_link_libraries(hello-ppu-audio-out PRIVATE
    sysutil_audio_out_stub audio_stub sysutil rt lv2 m 
)

ps3_add_self(hello-ppu-audio-out)
