# gemsample - PSL1GHT upstream sample ported to our CMake build.
#
# Source: PSL1GHT samples/input/gemsample @ 5ba78ae, verbatim.
# Original native Makefile replaced by this CMakeLists.

cmake_minimum_required(VERSION 3.20)
project(gemsample C)

include(ps3-self)

add_executable(gemsample
    source/camera.c
    source/gem.c
    source/main.c
    source/rsxutil.c)
target_compile_features(gemsample PRIVATE c_std_11)
target_include_directories(gemsample PRIVATE include)

# PSL1GHT Makefile LIBS: -lsimdmath -lrsx -lgcm_sys -lio -lsysutil -lrt -llv2 -lm -lcamera -lgem -lspurs -lsysmodule
target_link_libraries(gemsample PRIVATE
    simdmath rsx gcm_sys io sysutil rt lv2 m camera gem spurs sysmodule
)

ps3_add_self(gemsample)
