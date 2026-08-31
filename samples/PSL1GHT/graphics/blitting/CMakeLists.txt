# blitting - PSL1GHT upstream sample ported to our CMake build.
#
# Source: PSL1GHT samples/graphics/blitting @ 5ba78ae, verbatim.
# Original native Makefile replaced by this CMakeLists.

cmake_minimum_required(VERSION 3.20)
project(blitting CXX)

include(ps3-self)

add_executable(blitting
    source/bitmap.cpp
    source/main.cpp
    source/rsxutil.cpp)
target_compile_features(blitting PRIVATE cxx_std_17)
target_include_directories(blitting PRIVATE include)

# PSL1GHT Makefile LIBS: -lrsx -lgcm_sys -lio -lsysutil -lrt -llv2 -lm
target_link_libraries(blitting PRIVATE
    rsx gcm_sys io sysutil rt lv2 m
)

ps3_add_self(blitting)

ps3_add_pkg(blitting
    APPID     "PSLG00001"
    CONTENTID "UP0001-PSLG00001_00-PS3DKSAMPLES0000")
