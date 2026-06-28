# spuchain - PSL1GHT upstream sample ported to our CMake build.
#
# Source: PSL1GHT samples/spu/spuchain @ 5ba78ae, verbatim.
# Original native Makefiles replaced by this CMakeLists.

cmake_minimum_required(VERSION 3.20)
project(spuchain C ASM)

include(ps3-self)

add_executable(spuchain source/main.c)
target_compile_features(spuchain PRIVATE c_std_11)
target_include_directories(spuchain PRIVATE include)

# PSL1GHT Makefile LIBS: -lrsx -lgcm_sys -lio -lsysutil -lrt -llv2 -lm
target_link_libraries(spuchain PRIVATE
    rsx gcm_sys io sysutil rt lv2 m
)

ps3_add_spu_image(spuchain
    NAME spu
    SOURCES spu/source/main.c
    LIBS sputhread
    CFLAGS "-I${CMAKE_CURRENT_SOURCE_DIR}/include")

ps3_add_self(spuchain)
