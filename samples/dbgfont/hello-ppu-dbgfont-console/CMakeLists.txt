# hello-ppu-dbgfont-console — exercise the Console API subsystem.
#
# Opens a second console alongside the auto-created stdout console
# (CELL_DBGFONT_STDOUT_ID), writes lines to both, renders one frame,
# and tears down cleanly.
#
# Build:
#   cmake -S . -B build
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build
# Produces: build/hello-ppu-dbgfont-console{,.stripped.elf,.self,.fake.self}

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-dbgfont-console C)

include(ps3-self)

add_executable(hello-ppu-dbgfont-console source/main.c)
target_compile_features(hello-ppu-dbgfont-console PRIVATE c_std_11)

target_link_libraries(hello-ppu-dbgfont-console PRIVATE
    dbgfont gcm_cmd gcm_sys io sysutil rt lv2 m
)

ps3_add_self(hello-ppu-dbgfont-console)
