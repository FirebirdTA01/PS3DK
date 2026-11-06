cmake_minimum_required(VERSION 3.16)
project(ps3dk_prof_candidate_files C)
include(ps3-self)
get_filename_component(repo "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(PROFILER_ABI "ilp32" CACHE STRING "ilp32 or lp64; select matching toolchain file")
set(PROFILER_MODE "app" CACHE STRING "app or thread")
set(PROFILER_TEST_DIRECTORY "/dev_hdd0/tmp" CACHE STRING "Existing output directory; files are retained")
if(NOT PROFILER_ABI MATCHES "^(ilp32|lp64)$" OR NOT PROFILER_MODE MATCHES "^(app|thread)$")
  message(FATAL_ERROR "Select ilp32/lp64 and app/thread")
endif()
if(NOT PROFILER_TEST_DIRECTORY MATCHES "^/[A-Za-z0-9_/-]+$")
  message(FATAL_ERROR "Test directory must be an absolute PS3 path without quoting or traversal")
endif()
set(src "${repo}/sdk/libps3dkprof/src")
# Candidate-source row, intentionally NOT an installed-profiler archive test.
add_library(profiler-files-candidate STATIC
  "${src}/core/prof_core.c" "${src}/core/prof_drain.c" "${src}/pal/pal_lv2.c"
  "${src}/hooks/arcs.c" "${src}/core/prof_arcs_stream.c"
  "${src}/core/prof_arcs_export.c" "${src}/core/prof_arcs_file.c")
target_include_directories(profiler-files-candidate PUBLIC "${repo}/sdk/include" "${src}/core")
target_compile_definitions(profiler-files-candidate PUBLIC PS3DK_PROF=1 PS3DK_PROF_WITH_ARCS=1 PS3DK_PROF_PAL_LV2)
target_compile_options(profiler-files-candidate PRIVATE
  -std=c11 -O2 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections -fno-instrument-functions)
set(target profiler-files-${PROFILER_ABI}-${PROFILER_MODE})
add_executable(${target} main.c)
target_link_libraries(${target} PRIVATE profiler-files-candidate)
target_compile_options(${target} PRIVATE -std=c11 -O2 -g -Wall -Wextra -Werror -fno-instrument-functions)
target_compile_definitions(${target} PRIVATE
  "PROFILER_TEST_DIRECTORY=\"${PROFILER_TEST_DIRECTORY}\""
  "PROFILER_ABI_NAME=\"${PROFILER_ABI}\"" "PROFILER_MODE_NAME=\"${PROFILER_MODE}\"")
if(PROFILER_MODE STREQUAL "thread")
  target_compile_definitions(${target} PRIVATE PROFILER_TEST_THREAD=1)
endif()
if(PROFILER_ABI STREQUAL "lp64")
  target_compile_options(profiler-files-candidate PUBLIC -mlp64)
  target_link_options(${target} PRIVATE -mlp64 "-L${PS3DK}/ppu/lib/lp64")
  target_compile_definitions(${target} PRIVATE PROFILER_EXPECT_PTR_BYTES=8)
else()
  target_link_options(${target} PRIVATE "-L${PS3DK}/ppu/lib")
  target_compile_definitions(${target} PRIVATE PROFILER_EXPECT_PTR_BYTES=4)
endif()
# Guard the runtime provider rather than silently selecting GCC's mirror librt.
target_link_options(${target} PRIVATE -Wl,--gc-sections "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/${target}.map")
ps3_add_self(${target})
