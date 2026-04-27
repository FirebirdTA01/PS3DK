# hello-ppu-rtc — librtc end-to-end validation test.
#
# Validates that the librtc_stub.a archive emitted from
# tools/nidgen/nids/extracted/librtc_stub.yaml resolves the cellRtc
# entry points against the cellRtc SPRX module at runtime.

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-rtc C)

include(ps3-self)

add_executable(hello-ppu-rtc source/main.c)
target_compile_features(hello-ppu-rtc PRIVATE c_std_11)

target_link_libraries(hello-ppu-rtc PRIVATE
    rtc_stub sysmodule_stub io sysutil rt lv2
)

ps3_add_self(hello-ppu-rtc)
