# gcm-config-abi — CMake build.
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build
# Produces:  build/gcm-config-abi{,.stripped.elf,.self,.fake.self}

cmake_minimum_required(VERSION 3.20)
project(gcm-config-abi C)

include(ps3-self)

add_executable(gcm-config-abi
    source/main.c
    source/probe.c)
target_compile_features(gcm-config-abi PRIVATE c_std_11)

target_link_libraries(gcm-config-abi PRIVATE
    gcm_cmd gcm_sys io sysutil rt lv2 
)

ps3_add_self(gcm-config-abi)
