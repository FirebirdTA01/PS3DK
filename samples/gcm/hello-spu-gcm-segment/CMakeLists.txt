cmake_minimum_required(VERSION 3.20)
project(hello-spu-gcm-segment C ASM)
include(ps3-self)
add_executable(hello-spu-gcm-segment source/main.c)
target_compile_features(hello-spu-gcm-segment PRIVATE c_std_11)
target_compile_options(hello-spu-gcm-segment PRIVATE -O2 -Wall -Wextra -Werror)
target_link_libraries(hello-spu-gcm-segment PRIVATE gcm_cmd gcm_sys sysutil_stub sysutil rt lv2)
ps3_add_spu_image(hello-spu-gcm-segment
    NAME hello_spu_gcm_segment
    SOURCES spu/source/main.c
    LIBS gcm_spu_cmd sputhread)
ps3_add_self(hello-spu-gcm-segment)
ps3_add_pkg(hello-spu-gcm-segment
    APPID "GCMS00003" CONTENTID "UP0001-GCMS00003_00-PS3DKSAMPLES0000")
