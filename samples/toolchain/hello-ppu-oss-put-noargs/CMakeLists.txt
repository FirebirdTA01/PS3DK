cmake_minimum_required(VERSION 3.20)
project(hello-ppu-oss-put-noargs CXX)

include(ps3-self)

add_executable(hello-ppu-oss-put-noargs source/main.cpp)
target_compile_features(hello-ppu-oss-put-noargs PRIVATE cxx_std_17)

option(PS3DK_ENABLE_UNDEFINED_NULL_PROBE
    "Enable historical null-reference diagnostic (undefined behavior; may crash)" OFF)
if(PS3DK_ENABLE_UNDEFINED_NULL_PROBE)
    target_compile_definitions(hello-ppu-oss-put-noargs PRIVATE PS3DK_ENABLE_UNDEFINED_NULL_PROBE=1)
endif()
target_link_libraries(hello-ppu-oss-put-noargs PRIVATE sysutil m)

ps3_add_self(hello-ppu-oss-put-noargs)
