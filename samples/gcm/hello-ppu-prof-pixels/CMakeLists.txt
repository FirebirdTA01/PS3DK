# hello-ppu-prof-pixels — known answers for the RSX frame capture's passed samples and zcull state (README.md).
#
# Build (the three profiler options are required; the configure step refuses without them):
#   cmake -S . -B build -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake \
#     -DPS3_PROF_AUTO=ON -DPS3_PROF_AUTO_RING=ON -DPS3_PROF_AUTO_RING_MB=1 -DPS3_PROF_AUTO_RING_RECORDS=4096 \
#     -DPS3_PROF_RSX_FRAME=ON
#   cmake --build build
# Check a capture:
#   python3 tools/check_pixels.py --reader ps3dk-prof --capture <run>.prof [--tty TTY.log]

cmake_minimum_required(VERSION 3.21)
project(hello-ppu-prof-pixels C ASM)

include(ps3-self)

foreach(_opt PS3_PROF_AUTO PS3_PROF_AUTO_RING PS3_PROF_RSX_FRAME)
    if(NOT ${_opt})
        message(FATAL_ERROR "hello-ppu-prof-pixels needs -D${_opt}=ON (the build line at the top of CMakeLists.txt)")
    endif()
endforeach()

add_executable(hello-ppu-prof-pixels source/main.c)
target_compile_features(hello-ppu-prof-pixels PRIVATE c_std_11)
target_compile_definitions(hello-ppu-prof-pixels PRIVATE PS3DK_PROF=1)
target_link_libraries(hello-ppu-prof-pixels PRIVATE gcm_cmd gcm_sys io sysutil sysutil_stub sysmodule rt lv2 m)

foreach(_shader vp_quad.vcg fp_color.fcg fp_kil.fcg)
    ps3_add_cg_shader_rsxcgc(hello-ppu-prof-pixels shaders/${_shader})
endforeach()

ps3_add_self(hello-ppu-prof-pixels)

ps3_add_pkg(hello-ppu-prof-pixels
    TITLE     "PS3DK Pixel Counts"
    APPID     "GCMX00024"
    CONTENTID "UP0001-GCMX00024_00-PS3DKSAMPLES0000")
