# httptest - PSL1GHT upstream sample ported to our CMake build.
#
# Source: PSL1GHT samples/network/httptest @ 5ba78ae, verbatim.
# Original native Makefile replaced by this CMakeLists.

cmake_minimum_required(VERSION 3.20)
project(httptest C)

include(ps3-self)

add_executable(httptest source/http.c)
target_compile_features(httptest PRIVATE c_std_11)

# PSL1GHT Makefile LIBS: -lrsx -lgcm_sys -lio -lsysutil -lrt -llv2 -lm -lnet -lsysmodule -lssl -lhttp -lhttputil
target_link_libraries(httptest PRIVATE
    rsx gcm_sys io sysutil rt lv2 m net sysmodule ssl http httputil
)

ps3_add_self(httptest)
