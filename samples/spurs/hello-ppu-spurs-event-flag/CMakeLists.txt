# hello-ppu-spurs-event-flag — PPU-only Spurs sample (CMake build).
#
# The SPU policy-module + worker code lives inside libspurs (we
# rebuild via /ppu/lib/libspurs.a); the sample itself only
# drives the PPU-side Spurs surface.
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-spurs-event-flag CXX)

include(ps3-self)

add_executable(hello-ppu-spurs-event-flag source/main.cpp)
target_compile_features(hello-ppu-spurs-event-flag PRIVATE cxx_std_17)

target_link_libraries(hello-ppu-spurs-event-flag PRIVATE
    spurs_stub c_stub sysmodule_stub sysutil rt lv2 m
)

ps3_add_self(hello-ppu-spurs-event-flag)
