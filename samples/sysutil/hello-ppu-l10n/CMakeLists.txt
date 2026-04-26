# hello-ppu-l10n — CMake build (mirrors Makefile; both coexist until phase 7e).
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build
# Produces:  build/hello-ppu-l10n{,.stripped.elf,.self,.fake.self}

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-l10n C)

include(ps3-self)

add_executable(hello-ppu-l10n source/main.c)
target_compile_features(hello-ppu-l10n PRIVATE c_std_11)

target_link_libraries(hello-ppu-l10n PRIVATE
    l10n_stub sysmodule_stub sysutil rt lv2 
)

ps3_add_self(hello-ppu-l10n)
