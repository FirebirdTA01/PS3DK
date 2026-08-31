# padtest - PSL1GHT upstream sample ported to our CMake build.
#
# Source: PSL1GHT samples/input/padtest @ 5ba78ae, verbatim.
# Original native Makefile replaced by this CMakeLists.

cmake_minimum_required(VERSION 3.20)
project(padtest C)

include(ps3-self)

add_executable(padtest
    source/helper.c
    source/padtest.c)
target_compile_features(padtest PRIVATE c_std_11)

# PSL1GHT Makefile LIBS: -lio -lnet
target_link_libraries(padtest PRIVATE
    io net
)

ps3_add_self(padtest)

ps3_add_pkg(padtest
    APPID     "PSLG00004"
    CONTENTID "UP0001-PSLG00004_00-PS3DKSAMPLES0000")
