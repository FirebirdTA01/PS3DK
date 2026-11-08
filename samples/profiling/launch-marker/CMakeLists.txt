cmake_minimum_required(VERSION 3.20)
project(launch_marker C)
include(ps3-self)
# A DIAGNOSTIC sample (the clock probe's launch localization, variant L); unvalidated on target.
set(LM_BUILD_ID "" CACHE STRING "marker tag: 16 lowercase hex digits (main.c sha256 prefix); required")
include("${CMAKE_CURRENT_LIST_DIR}/lm_build_id.cmake")
add_executable(launch-marker source/main.c)
target_compile_features(launch-marker PRIVATE c_std_11)
target_compile_options(launch-marker PRIVATE -O2 -g -Wall -Wextra -Werror)
target_compile_definitions(launch-marker PRIVATE "LM_BUILD_ID=\"${LM_BUILD_ID}\"")
target_link_libraries(launch-marker PRIVATE sysutil sysmodule rt lv2 m)
ps3_add_self(launch-marker)
ps3_add_pkg(launch-marker
    APPID     "CLKMARK01"
    CONTENTID "UP0001-CLKMARK01_00-PS3DKSAMPLES0000")
