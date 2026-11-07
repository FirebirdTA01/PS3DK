cmake_minimum_required(VERSION 3.20)
project(overhead_bench C ASM)
include(ps3-self)
# overhead-bench (Plan A r2 3.1, M arm): ILP32 only.  A1: SOURCE ONLY; the objects are compiled for the build-time
# audit (3.1d), no image is linked or launched before the A2 gates.  The PPU side links the UNINSTALLED ILP32 ext
# archive (`make -C sdk/libps3dkprof ext-ilp32`, OB_EXT_ARCHIVE); the SPU program compiles the producer core and the
# writer.  The three PPU TUs share one loop source (source/bench_ppu.inc) and differ ONLY in PS3DK_PROF (per source
# below); every other flag is the target's.
set(OB_EXT_ARCHIVE "" CACHE FILEPATH "the uninstalled libps3dkprof_ext.a (make ext-ilp32); required")
set(OB_PROF_SRC "${CMAKE_CURRENT_LIST_DIR}/../../../sdk/libps3dkprof/src" CACHE PATH "libps3dkprof sources (headers)")
if(NOT EXISTS "${OB_EXT_ARCHIVE}")
  message(FATAL_ERROR "OB_EXT_ARCHIVE must name the uninstalled ILP32 ext archive (got '${OB_EXT_ARCHIVE}')")
endif()
get_filename_component(_ob_repo_root "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(_ob_path_maps "-ffile-prefix-map=${_ob_repo_root}=." "-fmacro-prefix-map=${_ob_repo_root}=.")
set(_ob_common common/ob_sched.c common/ob_cases.c common/ob_csv.c common/ob_teardown.c common/ob_rsx.c
  common/ob_io.c common/ob_spu_life.c common/ob_gpu_run.c common/ob_drv.c)
add_executable(overhead-bench source/main.c source/spu_bind.c source/rsx_bind.c
  source/bench_ppu_on.c source/bench_ppu_off.c source/bench_ppu_off_b.c ${_ob_common})
target_compile_features(overhead-bench PRIVATE c_std_11)
target_compile_options(overhead-bench PRIVATE -O2 -g -Wall -Wextra -Werror ${_ob_path_maps})
target_include_directories(overhead-bench PRIVATE "${OB_PROF_SRC}/../../include" "${CMAKE_CURRENT_LIST_DIR}/source"
  "${CMAKE_CURRENT_LIST_DIR}/../../gcm/common" "${OB_PROF_SRC}/spu" "${OB_PROF_SRC}/rsx" "${PS3DK}/ppu/include")
# the ON/OFF/OFF-b split (3.1a): the bindings and the driver are profiled (PS3DK_PROF=1)
set_source_files_properties(source/bench_ppu_on.c source/main.c source/spu_bind.c source/rsx_bind.c
  PROPERTIES COMPILE_DEFINITIONS "PS3DK_PROF=1")
set_source_files_properties(source/bench_ppu_off.c source/bench_ppu_off_b.c ${_ob_common}
  PROPERTIES COMPILE_DEFINITIONS "PS3DK_PROF=0")
target_link_libraries(overhead-bench PRIVATE "${OB_EXT_ARCHIVE}" gcm_cmd gcm_sys io sysutil sysmodule rt lv2 m)
set(OB_SPU_SDK "${PS3DK}" CACHE PATH "SPU SDK root")
# bench_spu_off.c resets PS3DK_PROF to 0 itself (the image's CFLAGS are shared by every SPU source)
ps3_add_spu_image(overhead-bench NAME ob_spu
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/spu/main.c" "${CMAKE_CURRENT_LIST_DIR}/spu/bench_spu_on.c"
          "${CMAKE_CURRENT_LIST_DIR}/spu/bench_spu_off.c" "${CMAKE_CURRENT_LIST_DIR}/common/ob_cases.c"
          "${OB_PROF_SRC}/spu/psp_spu.c" "${OB_PROF_SRC}/spu/prof_spu_writer.c"
  LIBS sputhread
  CFLAGS "-O2" "-DPS3DK_PROF=1" "-mno-error-reloc" ${_ob_path_maps} "-I${OB_PROF_SRC}/spu" "-I${OB_SPU_SDK}/spu/include"
  LDFLAGS "-L${OB_SPU_SDK}/spu/lib")
ps3_add_self(overhead-bench)
