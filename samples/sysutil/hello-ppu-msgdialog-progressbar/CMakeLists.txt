# hello-ppu-msgdialog-progressbar — CMake build.
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build
# Produces:  build/hello-ppu-msgdialog-progressbar{,.stripped.elf,.self,.fake.self}

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-msgdialog-progressbar C)

include(ps3-self)

add_executable(hello-ppu-msgdialog-progressbar source/main.c)
target_compile_features(hello-ppu-msgdialog-progressbar PRIVATE c_std_11)

target_link_libraries(hello-ppu-msgdialog-progressbar PRIVATE
    sysutil rt lv2
)

ps3_add_self(hello-ppu-msgdialog-progressbar)
