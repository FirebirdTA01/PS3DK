# sputest - PSL1GHT upstream sample ported to our CMake build.
#
# Source: PSL1GHT samples/spu/sputest @ 5ba78ae, verbatim.
# Original native Makefiles replaced by this CMakeLists.

cmake_minimum_required(VERSION 3.20)
project(sputest C ASM)

include(ps3-self)

add_executable(sputest source/main.c)
target_compile_features(sputest PRIVATE c_std_11)

# PSL1GHT Makefile LIBS: -lrsx -lgcm_sys -lio -lsysutil -lrt -llv2 -lm
target_link_libraries(sputest PRIVATE
    rsx gcm_sys io sysutil rt lv2 m
)

ps3_add_spu_image(sputest
    NAME spu
    SOURCES spu/source/main.c
    LIBS sputhread)

ps3_add_self(sputest)
