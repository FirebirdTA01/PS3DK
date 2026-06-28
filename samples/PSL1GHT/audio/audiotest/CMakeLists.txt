# audiotest - PSL1GHT upstream sample ported to our CMake build.
#
# Source: PSL1GHT samples/audio/audiotest @ 5ba78ae, verbatim.
# Original native Makefile replaced by this CMakeLists.

cmake_minimum_required(VERSION 3.20)
project(audiotest C)

include(ps3-self)

add_executable(audiotest source/main.c)
target_compile_features(audiotest PRIVATE c_std_11)

# PSL1GHT Makefile LIBS: -laudio -lm
target_link_libraries(audiotest PRIVATE
    audio m
)

ps3_add_self(audiotest)

ps3_add_pkg(audiotest
    TITLE     "Test Audio - PSL1GHT"
    APPID     "TESTAUDIO"
    CONTENTID "UP0001-TESTAUDIO_00-0000000000000000")
