# echoserv - PSL1GHT upstream sample ported to our CMake build.
#
# Source: PSL1GHT samples/network/echoserv @ 5ba78ae, verbatim.
# Original native Makefile replaced by this CMakeLists.

cmake_minimum_required(VERSION 3.20)
project(echoserv C)

include(ps3-self)

add_executable(echoserv
    source/echoserv.c
    source/helper.c)
target_compile_features(echoserv PRIVATE c_std_11)

# PSL1GHT Makefile LIBS: -lnet -lsysmodule
target_link_libraries(echoserv PRIVATE
    net sysmodule
)

ps3_add_self(echoserv)
