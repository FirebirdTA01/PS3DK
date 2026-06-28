# debugtest - PSL1GHT upstream sample ported to our CMake build.
#
# Source: PSL1GHT samples/network/debugtest @ 5ba78ae, verbatim.
# Original native Makefile replaced by this CMakeLists.

cmake_minimum_required(VERSION 3.20)
project(debugtest C)

include(ps3-self)

add_executable(debugtest source/main.c)
target_compile_features(debugtest PRIVATE c_std_11)

# PSL1GHT Makefile LIBS: -lrsx -lgcm_sys -lio -lsysutil -lrt -llv2 -lm -lnet -lsysmodule
target_link_libraries(debugtest PRIVATE
    rsx gcm_sys io sysutil rt lv2 m net sysmodule
)

ps3_add_self(debugtest)
