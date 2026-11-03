cmake_minimum_required(VERSION 3.20)
project(clock_calibration_w C ASM)
include(ps3-self)
# A DIAGNOSTIC sample: the c4 clock probe (../clock-calibration, its SPU program and ledger unchanged) plus CLKW1
# checkpoint markers.  W_VARIANT W or W0 (W0 omits the one decrementer write request).  Unvalidated on target.
set(W_VARIANT "W" CACHE STRING "W or W0")
set(W_BUILD_ID "" CACHE STRING "marker tag: 16 lowercase hex digits (main.c sha256 prefix); required")
include("${CMAKE_CURRENT_LIST_DIR}/w_build_id.cmake")
if(NOT W_VARIANT MATCHES "^(W|W0)$")
  message(FATAL_ERROR "W_VARIANT must be W or W0 (got '${W_VARIANT}')")
endif()
set(CAL_SPU_SAMPLES 1500 CACHE STRING "SPU samples per SPU phase (two phases)")
set(CAL_RSX_SAMPLES 1500 CACHE STRING "RSX samples")
set(CAL_PERIOD_US 10000 CACHE STRING "Microseconds between samples")
set(_c4 "${CMAKE_CURRENT_LIST_DIR}/../clock-calibration")
add_executable(clock-calibration-w source/main.c)
target_compile_features(clock-calibration-w PRIVATE c_std_11)
target_compile_options(clock-calibration-w PRIVATE -O2 -g -Wall -Wextra -Werror)
target_include_directories(clock-calibration-w PRIVATE "${_c4}/source" "${CMAKE_CURRENT_LIST_DIR}/../../gcm/common"
  "${PS3DK}/ppu/include")
target_compile_definitions(clock-calibration-w PRIVATE CAL_SPU_SAMPLES=${CAL_SPU_SAMPLES}
  CAL_RSX_SAMPLES=${CAL_RSX_SAMPLES} CAL_PERIOD_US=${CAL_PERIOD_US} "W_BUILD_ID=\"${W_BUILD_ID}\""
  $<$<STREQUAL:${W_VARIANT},W0>:W_VARIANT_W0>)
target_link_libraries(clock-calibration-w PRIVATE gcm_cmd gcm_sys io sysutil sysmodule rt lv2 m)
set(CAL_SPU_SDK "${PS3DK}" CACHE PATH "SPU SDK root")
ps3_add_spu_image(clock-calibration-w NAME clockcal_spu SOURCES "${_c4}/spu/main.c" LIBS sputhread
  CFLAGS "-I${CAL_SPU_SDK}/spu/include" LDFLAGS "-L${CAL_SPU_SDK}/spu/lib")
ps3_add_self(clock-calibration-w)
if(W_VARIANT STREQUAL "W0")
  ps3_add_pkg(clock-calibration-w APPID "CLKCALW00" CONTENTID "UP0001-CLKCALW00_00-PS3DKSAMPLES0000")
else()
  ps3_add_pkg(clock-calibration-w APPID "CLKCALW01" CONTENTID "UP0001-CLKCALW01_00-PS3DKSAMPLES0000")
endif()
