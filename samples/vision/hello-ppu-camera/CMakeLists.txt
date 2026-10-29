cmake_minimum_required(VERSION 3.20)
project(hello-ppu-camera C)

include(ps3-self)

add_executable(hello-ppu-camera source/main.c source/eye_capture.c source/eye_yuv.c source/eye_display.c)
target_compile_features(hello-ppu-camera PRIVATE c_std_11)
target_compile_options(hello-ppu-camera PRIVATE -O2 -Wall -Wextra -Werror)

target_link_libraries(hello-ppu-camera PRIVATE
    camera_stub sysmodule_stub c_stub gcm_cmd gcm_sys sysutil_stub sysutil rt lv2
)

ps3_add_self(hello-ppu-camera OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
