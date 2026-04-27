# hello-ppu-spurs-job-chain — PPU-only Spurs job-chain sample (CMake build).
#
# The SPU job-chain runtime + dispatcher live inside libspurs (we
# rebuild via /ppu/lib/libspurs_stub.a); the sample itself only
# drives the PPU-side cell/spurs/job_chain.h surface.
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-spurs-job-chain CXX)

include(ps3-self)

add_executable(hello-ppu-spurs-job-chain source/main.cpp)
target_compile_features(hello-ppu-spurs-job-chain PRIVATE cxx_std_17)

target_link_libraries(hello-ppu-spurs-job-chain PRIVATE
    spurs_stub c_stub sysmodule_stub sysutil rt lv2 m
)

ps3_add_self(hello-ppu-spurs-job-chain)
