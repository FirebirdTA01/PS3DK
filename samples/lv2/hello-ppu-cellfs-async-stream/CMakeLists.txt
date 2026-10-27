# hello-ppu-cellfs-async-stream - CMake build.
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build
# Produces:  build/hello-ppu-cellfs-async-stream{,.stripped.elf,.self,.fake.self}

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-cellfs-async-stream C)

include(ps3-self)

add_executable(hello-ppu-cellfs-async-stream source/main.c)
target_compile_features(hello-ppu-cellfs-async-stream PRIVATE c_std_11)

target_link_libraries(hello-ppu-cellfs-async-stream PRIVATE
    fs_stub rt lv2
)

ps3_add_self(hello-ppu-cellfs-async-stream)
