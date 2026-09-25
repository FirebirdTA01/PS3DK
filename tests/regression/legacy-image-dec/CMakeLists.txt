cmake_minimum_required(VERSION 3.20)

project(legacy_image_dec_regression C ASM)

option(PS3TC_REGRESSION_HOST_STUB "Build with host stubs for CI syntax/link coverage" OFF)

if(PS3TC_REGRESSION_HOST_STUB)
    message(STATUS "legacy-image-dec: skipped under PS3TC_REGRESSION_HOST_STUB")
    return()
endif()

include(ps3-self)

add_executable(legacy-image-dec main.c)
target_compile_features(legacy-image-dec PRIVATE c_std_11)

target_link_libraries(legacy-image-dec PRIVATE
    jpgdec pngdec sysmodule sysutil lv2 rt m
)

ps3_bin2s(legacy-image-dec data/test.jpg)
ps3_bin2s(legacy-image-dec data/test_rgba.png)
ps3_bin2s(legacy-image-dec data/test_rgb.png)

ps3_add_self(legacy-image-dec)
