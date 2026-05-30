# hello-psgl-link — PSGL link-gate sample (Slice 1d acceptance gate).
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build
#
# Validates: #include <PSGL/psgl.h> compiles + links against libPSGL.a
# and libPSGLU.a.  The resulting .self does not need to run; gate is
# compile + link only.

cmake_minimum_required(VERSION 3.20)
project(hello-psgl-link CXX)

include(ps3-self)

add_executable(hello-psgl-link source/main.cpp)
target_compile_features(hello-psgl-link PRIVATE cxx_std_17)
target_link_libraries(hello-psgl-link PRIVATE PSGL PSGLU)

ps3_add_self(hello-psgl-link)
