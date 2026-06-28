# networktest - PSL1GHT upstream sample ported to our CMake build.
#
# Source: PSL1GHT samples/network/networktest @ 5ba78ae, verbatim.
# Original native Makefile replaced by this CMakeLists.

cmake_minimum_required(VERSION 3.20)
project(networktest C)

include(ps3-self)

add_executable(networktest source/main.c)
target_compile_features(networktest PRIVATE c_std_11)

# PSL1GHT Makefile LIBS: -lnet -lsysmodule
target_link_libraries(networktest PRIVATE
    net sysmodule
)

ps3_add_self(networktest)
