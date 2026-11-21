# hello-ppu-prof-auto-verdicts — known-answer phases for AUTO frame verdicts.
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake \
#     -DPS3_PROF_AUTO=ON \
#     -DPS3_PROF_AUTO_ROOT=<sdk-root>
#   cmake --build build

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-prof-auto-verdicts C ASM)

# The phases are known answers for the automatic RSX observer; built without it the sample records nothing and its
# AUTOVERDICT lines have no capture to check against, so it refuses here instead of building that.
if(NOT PS3_PROF_AUTO)
    message(FATAL_ERROR
        "hello-ppu-prof-auto-verdicts needs automatic RSX observation: configure it with -DPS3_PROF_AUTO=ON "
        "(and -DPS3_PROF_AUTO_ROOT=<sdk-root> when the providers are not in PS3DK). "
        "PS3_PROF_AUTO is off in this build directory.")
endif()

include(ps3-self)

add_executable(hello-ppu-prof-auto-verdicts source/main.c)
target_compile_features(hello-ppu-prof-auto-verdicts PRIVATE c_std_11)
target_compile_definitions(hello-ppu-prof-auto-verdicts PRIVATE PS3DK_PROF=1)

target_link_libraries(hello-ppu-prof-auto-verdicts PRIVATE
    gcm_cmd gcm_sys io sysutil sysutil_stub sysmodule rt lv2 m
)

ps3_add_cg_shader_rsxcgc(hello-ppu-prof-auto-verdicts shaders/vpshader.vcg)
ps3_add_cg_shader_rsxcgc(hello-ppu-prof-auto-verdicts shaders/fpshader.fcg)

ps3_add_self(hello-ppu-prof-auto-verdicts)

ps3_add_pkg(hello-ppu-prof-auto-verdicts
    APPID     "GCMX00023"
    CONTENTID "UP0001-GCMX00023_00-PS3DKSAMPLES0000")
