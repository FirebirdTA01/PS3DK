# hello-ppu-imejp - CMake build.
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build
# Produces:  build/hello-ppu-imejp{,.stripped.elf,.self,.fake.self}

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-imejp C)

include(ps3-self)

add_executable(hello-ppu-imejp source/main.c)
target_compile_features(hello-ppu-imejp PRIVATE c_std_11)

target_link_libraries(hello-ppu-imejp PRIVATE
    sysutil_imejp_stub sysmodule_stub io sysutil rt lv2
)

ps3_add_self(hello-ppu-imejp)
