# audiosync - PSL1GHT upstream sample ported to our CMake build.
#
# Source: PSL1GHT samples/audio/audiosync @ 5ba78ae, verbatim.
# Original native Makefile replaced by this CMakeLists.

cmake_minimum_required(VERSION 3.20)
project(audiosync C ASM)

include(ps3-self)

add_executable(audiosync source/main.c)
target_compile_features(audiosync PRIVATE c_std_11)

# PSL1GHT Makefile LIBS: -laudio -lrt -llv2 -lm
target_link_libraries(audiosync PRIVATE
    audio rt lv2 m
)

ps3_bin2s(audiosync data/tada48_16_2.raw)
ps3_add_self(audiosync)
