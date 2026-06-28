# spurs - PSL1GHT upstream sample ported to our CMake build.
#
# Source: PSL1GHT samples/spu/spurs @ 5ba78ae, verbatim.
# Original native Makefile replaced by this CMakeLists.

cmake_minimum_required(VERSION 3.20)
project(spurs C)

include(ps3-self)

add_executable(spurs source/main.c)
target_compile_features(spurs PRIVATE c_std_11)

# PSL1GHT Makefile LIBS: -lsimdmath -lrsx -lgcm_sys -lio -lsysutil -lrt -llv2 -lm -lcamera -lgem -lspurs -lsysmodule
target_link_libraries(spurs PRIVATE
    simdmath rsx gcm_sys io sysutil rt lv2 m camera gem -lspurs sysmodule
)

ps3_add_self(spurs)
