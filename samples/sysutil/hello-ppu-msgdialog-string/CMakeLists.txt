# hello-ppu-msgdialog-string — CMake build.
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build
# Produces:  build/hello-ppu-msgdialog-string{,.stripped.elf,.self,.fake.self}

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-msgdialog-string C)

include(ps3-self)

add_executable(hello-ppu-msgdialog-string source/main.c)
target_compile_features(hello-ppu-msgdialog-string PRIVATE c_std_11)

target_link_libraries(hello-ppu-msgdialog-string PRIVATE
    sysutil rt lv2
)

ps3_add_self(hello-ppu-msgdialog-string)
