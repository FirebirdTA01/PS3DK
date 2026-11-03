cmake_minimum_required(VERSION 3.20)
project(ext_probe C ASM)
include(ps3-self)
# E1: the first on-console SPU-lane + RSX-domain probe (EXTP1 markers).  ILP32 only.  UNBUILT and UNVALIDATED ON
# TARGET: the target build is its own checkpoint (build slot, pins), and the extension is NOT installed: the PPU side
# links the UNINSTALLED ILP32 ext archive from `make -C sdk/libps3dkprof ext-ilp32` (E1_EXT_ARCHIVE) and takes the
# internal headers from the reviewed tree (E1_PROF_SRC); the SPU program compiles the producer core and the writer.
set(E1_BUILD_ID "" CACHE STRING "marker tag: 16 lowercase hex digits (main.c sha256 prefix); required")
set(E1_EXT_ARCHIVE "" CACHE FILEPATH "the uninstalled libps3dkprof_ext.a (make ext-ilp32); required")
set(E1_PROF_SRC "${CMAKE_CURRENT_LIST_DIR}/../../../sdk/libps3dkprof/src" CACHE PATH "libps3dkprof sources (headers)")
string(LENGTH "${E1_BUILD_ID}" _e1_id_len)
if(NOT E1_BUILD_ID MATCHES "^[0-9a-f]+$" OR NOT _e1_id_len EQUAL 16)
  message(FATAL_ERROR "E1_BUILD_ID must be exactly 16 lowercase hex digits (got '${E1_BUILD_ID}')")
endif()
if(NOT EXISTS "${E1_EXT_ARCHIVE}")
  message(FATAL_ERROR "E1_EXT_ARCHIVE must name the uninstalled ILP32 ext archive (got '${E1_EXT_ARCHIVE}')")
endif()
add_executable(ext-probe source/main.c)
target_compile_features(ext-probe PRIVATE c_std_11)
target_compile_options(ext-probe PRIVATE -O2 -g -Wall -Wextra -Werror)
target_include_directories(ext-probe PRIVATE "${E1_PROF_SRC}/../../include" "${CMAKE_CURRENT_LIST_DIR}/source" "${CMAKE_CURRENT_LIST_DIR}/../../gcm/common"
  "${E1_PROF_SRC}/spu" "${E1_PROF_SRC}/rsx" "${PS3DK}/ppu/include")
target_compile_definitions(ext-probe PRIVATE PS3DK_PROF=1 "E1_BUILD_ID=\"${E1_BUILD_ID}\"")
target_link_libraries(ext-probe PRIVATE "${E1_EXT_ARCHIVE}" gcm_cmd gcm_sys io sysutil sysmodule rt lv2 m)
set(E1_SPU_SDK "${PS3DK}" CACHE PATH "SPU SDK root")
ps3_add_spu_image(ext-probe NAME e1_spu
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/spu/main.c" "${CMAKE_CURRENT_LIST_DIR}/spu/e1_spu_core.c"
          "${E1_PROF_SRC}/spu/psp_spu.c" "${E1_PROF_SRC}/spu/prof_spu_writer.c"
  LIBS sputhread
  CFLAGS "-DPS3DK_PROF=1" "-mno-error-reloc" "-I${E1_PROF_SRC}/spu" "-I${CMAKE_CURRENT_LIST_DIR}/source" "-I${E1_SPU_SDK}/spu/include"
  LDFLAGS "-L${E1_SPU_SDK}/spu/lib")
ps3_add_self(ext-probe)
ps3_add_pkg(ext-probe APPID "EXTPROBE1" CONTENTID "UP0001-EXTPROBE1_00-PS3DKSAMPLES0000")
