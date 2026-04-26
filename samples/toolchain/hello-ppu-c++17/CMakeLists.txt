# hello-ppu-c++17 — CMake build template for the PS3 PPU.
#
# This is the MVP / proof of concept for the CMake migration.  Once
# every sample has CMake parity, the parallel Makefile + ppu_rules
# include path goes away.  Until then, both build systems coexist;
# the Makefile is still authoritative.
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build
# Produces:  build/hello-ppu-c++17{,.stripped.elf,.self,.fake.self}
#
# Run on RPCS3:  rpcs3 --no-gui build/hello-ppu-c++17.fake.self

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-c++17 CXX)

# ps3-self.cmake provides ps3_add_self(target).  CMAKE_MODULE_PATH was
# extended by the toolchain file to include this repo's cmake/ dir.
include(ps3-self)

add_executable(hello-ppu-c++17 source/main.cpp)
target_compile_features(hello-ppu-c++17 PRIVATE cxx_std_17)

# The PPU GCC driver auto-injects sysbase / c / rt / lv2 via
# LIB_LV2_SPEC (see patches/ppu/gcc-12.x/0002-add-cell64lv2-target-header).
# We only need the EXTRA libs that the sample uses on top: libsysutil
# for sysutil callbacks, libm for libstdc++'s math primitives.
target_link_libraries(hello-ppu-c++17 PRIVATE sysutil m)

ps3_add_self(hello-ppu-c++17)
