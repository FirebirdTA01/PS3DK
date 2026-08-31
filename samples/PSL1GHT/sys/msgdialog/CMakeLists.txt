# msgdialog - PSL1GHT upstream sample ported to our CMake build.
#
# Source: PSL1GHT samples/sys/msgdialog @ 5ba78ae, verbatim.
# Original native Makefile replaced by this CMakeLists.

cmake_minimum_required(VERSION 3.20)
project(msgdialog CXX)

include(ps3-self)

add_executable(msgdialog
    source/main.cpp
    source/rsxutil.cpp)
target_compile_features(msgdialog PRIVATE cxx_std_17)
target_include_directories(msgdialog PRIVATE include)

# PSL1GHT Makefile LIBS: -lrsx -lgcm_sys -lio -lsysutil -lrt -llv2 -lm
target_link_libraries(msgdialog PRIVATE
    rsx gcm_sys io sysutil rt lv2 m
)

ps3_add_self(msgdialog)

ps3_add_pkg(msgdialog
    APPID     "PSLG00002"
    CONTENTID "UP0001-PSLG00002_00-PS3DKSAMPLES0000")
