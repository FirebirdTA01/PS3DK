cmake_minimum_required(VERSION 3.16)
project(ps3dk_prof_auto_lifecycle C CXX)
include(ps3-self)
get_filename_component(repo "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(PROFILER_ABI "ilp32" CACHE STRING "ilp32 or lp64")
if(NOT PROFILER_ABI MATCHES "^(ilp32|lp64)$")
  message(FATAL_ERROR "PROFILER_ABI must be ilp32 or lp64")
endif()
set(target profiler-auto-lifecycle-${PROFILER_ABI})
add_library(auto-runtime STATIC
  "${repo}/sdk/libps3dkprof/src/core/prof_core.c"
  "${repo}/sdk/libps3dkprof/src/core/prof_drain.c"
  "${repo}/sdk/libps3dkprof/src/pal/pal_lv2.c"
  "${repo}/sdk/libps3dkprof/src/auto/startup.c")
target_compile_definitions(auto-runtime PRIVATE PS3DK_PROF=1 PS3DK_PROF_PAL_LV2)
target_include_directories(auto-runtime PRIVATE "${repo}/sdk/include")
target_compile_options(auto-runtime PRIVATE -std=c11 -O2 -g -Wall -Wextra -Werror
  -Wno-prio-ctor-dtor -fno-instrument-functions)
add_executable(${target} "${repo}/tests/host/ps3dk-prof-runtime/auto_lifecycle.cpp")
target_include_directories(${target} PRIVATE "${repo}/sdk/include")
target_compile_options(${target} PRIVATE -std=c++17 -O2 -g -Wall -Wextra -Werror -Wno-prio-ctor-dtor)
target_link_libraries(${target} PRIVATE auto-runtime)
target_link_options(${target} PRIVATE -Wl,-u,__ps3dk_prof_startup_anchor
  "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/${target}.map")
# Force the selected installed SDK runtime ahead of GCC's sysroot mirror.
if(PROFILER_ABI STREQUAL "lp64")
  target_compile_options(auto-runtime PRIVATE -mlp64)
  target_compile_options(${target} PRIVATE -mlp64)
  target_link_options(${target} PRIVATE -mlp64 "-L$ENV{PS3DK}/ppu/lib/lp64")
else()
  target_link_options(${target} PRIVATE "-L$ENV{PS3DK}/ppu/lib")
endif()
ps3_add_self(${target})
