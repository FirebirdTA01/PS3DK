cmake_minimum_required(VERSION 3.20)
project(hello-ppu-fw-gcm-text CXX)

include(ps3-self)

add_executable(hello-ppu-fw-gcm-text
    source/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/FWNullFramework.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/FWGCMFramework.cpp)

target_compile_features(hello-ppu-fw-gcm-text PRIVATE cxx_std_17)
target_compile_options(hello-ppu-fw-gcm-text PRIVATE -W -Wall)
target_include_directories(hello-ppu-fw-gcm-text PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(hello-ppu-fw-gcm-text PRIVATE
    dbgfont rsx gcm_cmd gcm_sys io sysutil rt lv2 m)

ps3_add_self(hello-ppu-fw-gcm-text)
