# rsx-frame — a known draw matrix for the RSX frame capture, checked field by field (README.md).
#
# Build (all four profiler options are required; the configure step refuses without them):
#   cmake -S . -B build -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake \
#     -DPS3_PROF_AUTO=ON -DPS3_PROF_AUTO_RING=ON -DPS3_PROF_AUTO_RING_MB=1 -DPS3_PROF_AUTO_RING_RECORDS=4096 \
#     -DPS3_PROF_RSX_FRAME=ON -DPS3_PROF_LIVE_AUTOSTART=ON
#   cmake --build build
# Check a capture:
#   python3 tools/check_capture.py --reader ps3dk-prof --capture <run>.prof --shaders build/shaders

cmake_minimum_required(VERSION 3.21)
project(rsx-frame C ASM)

include(ps3-self)

foreach(_opt PS3_PROF_AUTO PS3_PROF_AUTO_RING PS3_PROF_RSX_FRAME PS3_PROF_LIVE_AUTOSTART)
    if(NOT ${_opt})
        message(FATAL_ERROR "rsx-frame needs -D${_opt}=ON (the build line at the top of CMakeLists.txt)")
    endif()
endforeach()

add_executable(rsx-frame source/main.c)
target_compile_features(rsx-frame PRIVATE c_std_11)
target_compile_definitions(rsx-frame PRIVATE PS3DK_PROF=1)
target_link_libraries(rsx-frame PRIVATE gcm_cmd gcm_sys io sysutil sysutil_stub sysmodule rt lv2 m)

# the expected table names programs by these stems (build/shaders/<stem>.vpo / .fpo)
foreach(_shader vp_offset.vcg vp_scale.vcg fp_color.fcg fp_tex.fcg fp_texmod.fcg)
    ps3_add_cg_shader_rsxcgc(rsx-frame shaders/${_shader})
endforeach()

ps3_add_self(rsx-frame)

ps3_add_pkg(rsx-frame
    TITLE     "PS3DK RSX Frame Capture"
    APPID     "RSXFRAME1"
    CONTENTID "UP0001-RSXFRAME1_00-PS3DKSAMPLES0000")
