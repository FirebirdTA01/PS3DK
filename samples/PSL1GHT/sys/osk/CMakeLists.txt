# osk - PSL1GHT upstream sample ported to our CMake build.
#
# Source: PSL1GHT samples/sys/osk @ 5ba78ae, verbatim.
# Original native Makefile replaced by this CMakeLists.

cmake_minimum_required(VERSION 3.20)
project(osk CXX)

include(ps3-self)

add_executable(osk
    source/main.cpp
    source/rsxutil.cpp)
target_compile_features(osk PRIVATE cxx_std_17)
target_include_directories(osk PRIVATE include)

# PSL1GHT Makefile LIBS: -lrsx -lgcm_sys -lio -lsysutil -lrt -llv2 -lm
target_link_libraries(osk PRIVATE
    rsx gcm_sys io sysutil rt lv2 m
)

ps3_add_self(osk)
