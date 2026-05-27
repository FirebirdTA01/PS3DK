cmake_minimum_required(VERSION 3.20)

project(hello-ppu-fw-null-lifecycle CXX)

include(ps3-self)

add_executable(hello-ppu-fw-null-lifecycle
    source/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/FWNullFramework.cpp)

target_compile_features(hello-ppu-fw-null-lifecycle PRIVATE cxx_std_17)
target_compile_options(hello-ppu-fw-null-lifecycle PRIVATE -W -Wall)
target_include_directories(hello-ppu-fw-null-lifecycle PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(hello-ppu-fw-null-lifecycle PRIVATE rt lv2 m)

ps3_add_self(hello-ppu-fw-null-lifecycle)
