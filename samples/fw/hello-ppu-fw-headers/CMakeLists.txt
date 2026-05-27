cmake_minimum_required(VERSION 3.20)
project(hello-ppu-fw-headers CXX)

include(ps3-self)

add_executable(hello-ppu-fw-headers source/main.cpp)
target_compile_features(hello-ppu-fw-headers PRIVATE cxx_std_17)
target_include_directories(hello-ppu-fw-headers PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_compile_options(hello-ppu-fw-headers PRIVATE -W -Wall)
target_link_libraries(hello-ppu-fw-headers PRIVATE rt lv2 m)

ps3_add_self(hello-ppu-fw-headers)
