# hello-ppu-jpg-dec — CMake build (mirrors Makefile; both coexist until phase 7e).
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build
# Produces:  build/hello-ppu-jpg-dec{,.stripped.elf,.self,.fake.self}

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-jpg-dec C ASM)

include(ps3-self)

add_executable(hello-ppu-jpg-dec source/main.c)
target_compile_features(hello-ppu-jpg-dec PRIVATE c_std_11)

target_link_libraries(hello-ppu-jpg-dec PRIVATE
    jpgdec_stub sysmodule_stub gcm_sys rsx io sysutil rt lv2 m 
)

ps3_bin2s(hello-ppu-jpg-dec data/test.jpg)

ps3_add_self(hello-ppu-jpg-dec)
