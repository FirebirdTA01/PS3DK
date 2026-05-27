cmake_minimum_required(VERSION 3.20)
project(hello-ppu-fw-input CXX)

include(ps3-self)

add_executable(hello-ppu-fw-input
    source/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/FWNullFramework.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/FWGCMFramework.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/FWCellInput.cpp)

target_compile_features(hello-ppu-fw-input PRIVATE cxx_std_17)
target_compile_options(hello-ppu-fw-input PRIVATE -W -Wall)
target_include_directories(hello-ppu-fw-input PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(hello-ppu-fw-input PRIVATE
    dbgfont rsx gcm_cmd gcm_sys io_stub io sysutil rt lv2 m)

ps3_add_self(hello-ppu-fw-input)
