# spuparallel - PSL1GHT upstream sample ported to our CMake build.
#
# Source: PSL1GHT samples/spu/spuparallel @ 5ba78ae, verbatim.
# Original native Makefiles replaced by this CMakeLists.

cmake_minimum_required(VERSION 3.20)
project(spuparallel C ASM)

include(ps3-self)

add_executable(spuparallel source/main.c)
target_compile_features(spuparallel PRIVATE c_std_11)
target_include_directories(spuparallel PRIVATE include)

# PSL1GHT Makefile LIBS: -lrsx -lgcm_sys -lio -lsysutil -lrt -llv2 -lm
target_link_libraries(spuparallel PRIVATE
    rsx gcm_sys io sysutil rt lv2 m
)

ps3_add_spu_image(spuparallel
    NAME spu
    SOURCES spu/source/main.c
    LIBS sputhread
    CFLAGS "-I${CMAKE_CURRENT_SOURCE_DIR}/include")

ps3_add_self(spuparallel)
