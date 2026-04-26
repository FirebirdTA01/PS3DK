# hello-ppu-backfill — CMake build.
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build
# Produces:  build/hello-ppu-backfill{,.stripped.elf,.self,.fake.self}

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-backfill C)

include(ps3-self)

add_executable(hello-ppu-backfill source/main.c)
target_compile_features(hello-ppu-backfill PRIVATE c_std_11)

target_link_libraries(hello-ppu-backfill PRIVATE
    sysutil_subdisplay_stub sysutil_music_stub sysutil_music_decode_stub sysutil_music_export_stub sysutil_imejp_stub sysutil rt lv2 
)

ps3_add_self(hello-ppu-backfill)
