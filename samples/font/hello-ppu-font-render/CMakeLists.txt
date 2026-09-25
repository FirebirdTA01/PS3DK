# hello-ppu-font-render — CMake build.
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build
# Produces:  build/hello-ppu-font-render{,.stripped.elf,.self,.fake.self}
#
# Renders text with the system font through the canonical cellFont API and
# through PSL1GHT's font* names, shows it, and writes the displayed buffer
# to /dev_hdd0/tmp/hello-ppu-font-render.bmp.

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-font-render C)

include(ps3-self)

add_executable(hello-ppu-font-render source/main.c source/psl1ght_font.c)
target_compile_features(hello-ppu-font-render PRIVATE c_std_11)

target_link_libraries(hello-ppu-font-render PRIVATE
    fontFT_stub font_stub fs_stub sysmodule_stub gcm_sys rsx io sysutil rt lv2 m
)

ps3_add_self(hello-ppu-font-render)
