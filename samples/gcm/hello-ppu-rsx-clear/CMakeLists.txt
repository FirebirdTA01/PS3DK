# hello-ppu-rsx-clear — CMake build.
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build
# Produces:  build/hello-ppu-rsx-clear{,.stripped.elf,.self,.fake.self}

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-rsx-clear C)

include(ps3-self)

add_executable(hello-ppu-rsx-clear source/main.c)
target_compile_features(hello-ppu-rsx-clear PRIVATE c_std_11)

target_link_libraries(hello-ppu-rsx-clear PRIVATE
    rsx gcm_sys io sysutil rt lv2 
)

ps3_add_self(hello-ppu-rsx-clear)
