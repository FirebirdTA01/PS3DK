# kbtest - PSL1GHT upstream sample ported to our CMake build.
#
# Source: PSL1GHT samples/input/kbtest @ 5ba78ae, verbatim.
# Original native Makefile replaced by this CMakeLists.

cmake_minimum_required(VERSION 3.20)
project(kbtest C)

include(ps3-self)

add_executable(kbtest
    source/helper.c
    source/kbtest.c)
target_compile_features(kbtest PRIVATE c_std_11)

# PSL1GHT Makefile LIBS: -lio -lnet
target_link_libraries(kbtest PRIVATE
    io net
)

ps3_add_self(kbtest)

ps3_add_pkg(kbtest
    APPID     "PSLG00005"
    CONTENTID "UP0001-PSLG00005_00-PS3DKSAMPLES0000")
