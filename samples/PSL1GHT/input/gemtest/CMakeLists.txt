# gemtest - PSL1GHT upstream sample ported to our CMake build.
#
# Source: PSL1GHT samples/input/gemtest @ 5ba78ae, verbatim.
# Original native Makefile replaced by this CMakeLists.

cmake_minimum_required(VERSION 3.20)
project(gemtest C)

include(ps3-self)

add_executable(gemtest source/main.c)
target_compile_features(gemtest PRIVATE c_std_11)

# PSL1GHT Makefile LIBS: -lgcm_sys -lrsx -lsysutil -lio -lcamera -lsysmodule -ljpgdec -lgem -llv2 -lrt
target_link_libraries(gemtest PRIVATE
    gcm_sys rsx sysutil io camera sysmodule jpgdec gem lv2 rt
)

ps3_add_self(gemtest)
