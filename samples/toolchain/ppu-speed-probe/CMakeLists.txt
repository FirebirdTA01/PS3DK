# ppu-speed-probe: timebase speed probe for comparing two builds on hardware.
#
# Build:
#   cmake -S . -B build -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build
# Run: prints one "BENCH <kernel> min <ticks> med <ticks> ..." line per kernel
# and "BENCH_DONE" on the TTY.  Compare two builds by the min ticks of each
# kernel over several interleaved runs; the sums must be equal.

cmake_minimum_required(VERSION 3.20)
project(ppu-speed-probe CXX)

include(ps3-self)

set(_pl "$ENV{PS3DK}/portlibs/ppu")
if(NOT EXISTS "${_pl}/lib/libz.a")
  set(_pl "$ENV{PS3DEV}/portlibs/ppu")
endif()

add_executable(ppu-speed-probe source/main.cpp source/micro.cpp)
target_compile_features(ppu-speed-probe PRIVATE cxx_std_17)
target_compile_options(ppu-speed-probe PRIVATE -O2)
target_include_directories(ppu-speed-probe PRIVATE "${_pl}/include" "${_pl}/include/pixman-1")
target_link_libraries(ppu-speed-probe PRIVATE
    "${_pl}/lib/libpixman-1.a" "${_pl}/lib/libpng16.a" "${_pl}/lib/libz.a" m rt lv2)

ps3_add_self(ppu-speed-probe)
