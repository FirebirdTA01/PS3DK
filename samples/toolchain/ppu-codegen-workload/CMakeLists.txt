# ppu-codegen-workload: artificial frame-based workload for comparing two
# builds on hardware by frame rate (one phase per code-generation pattern).
#
# Build:
#   cmake -S . -B build -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake [-DPS3_PROF_AUTO=ON]
#   cmake --build build
# With -DPS3_PROF_AUTO=ON the whole run is captured to /dev_hdd0/tmp.

cmake_minimum_required(VERSION 3.20)
project(ppu-codegen-workload CXX)

include(ps3-self)

set(PHASE_FRAMES 300 CACHE STRING "Frames per phase per round")
set(ROUNDS 2 CACHE STRING "Rounds through all phases")

add_executable(ppu-codegen-workload source/main.cpp source/work.cpp)
target_compile_features(ppu-codegen-workload PRIVATE cxx_std_17)
target_compile_options(ppu-codegen-workload PRIVATE -O2)
target_compile_definitions(ppu-codegen-workload PRIVATE
    PHASE_FRAMES=${PHASE_FRAMES} ROUNDS=${ROUNDS})
target_link_libraries(ppu-codegen-workload PRIVATE
    gcm_cmd gcm_sys io sysutil rt lv2)

ps3_add_self(ppu-codegen-workload)
