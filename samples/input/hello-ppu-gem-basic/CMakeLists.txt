cmake_minimum_required(VERSION 3.20)
project(hello-ppu-gem-basic CXX)
include(ps3-self)
add_executable(hello-ppu-gem-basic source/main.cpp source/gem_session.cpp)
target_compile_features(hello-ppu-gem-basic PRIVATE cxx_std_17)
target_compile_options(hello-ppu-gem-basic PRIVATE -O2 -Wall -Wextra -Werror)
target_link_libraries(hello-ppu-gem-basic PRIVATE
    gem_stub spurs_stub sysmodule_stub c_stub sysutil_stub sysutil rt lv2 m)
ps3_add_self(hello-ppu-gem-basic OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
