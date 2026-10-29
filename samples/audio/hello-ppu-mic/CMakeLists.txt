cmake_minimum_required(VERSION 3.20)
project(hello-ppu-mic CXX)

include(ps3-self)

add_executable(hello-ppu-mic source/main.cpp source/mic_meter.cpp source/mic_session.cpp)
target_compile_features(hello-ppu-mic PRIVATE cxx_std_17)
target_compile_options(hello-ppu-mic PRIVATE -O2 -Wall -Wextra -Werror)

target_link_libraries(hello-ppu-mic PRIVATE
    mic_stub sysutil_avconf_ext_stub sysmodule_stub sysutil_stub c_stub sysutil rt lv2
)

ps3_add_self(hello-ppu-mic OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
