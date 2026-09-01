# shader-differential — A/B pixel-differential rig (regression battery row).
#
# Judges pairs of compiled fragment-program containers staged to
# /dev_hdd0 at runtime: renders both sides into off-screen RTs in the
# same run and raw-byte compares.  Only the shared pass-through VP is
# embedded at build time — pair containers (including any
# reference-compiled material, which never enters the tree) are
# runtime input staged by stage-differential.ps1.

cmake_minimum_required(VERSION 3.20)

project(shader_differential_regression C ASM)

option(PS3TC_REGRESSION_HOST_STUB "Build with host stubs for CI syntax/link coverage" OFF)

if(PS3TC_REGRESSION_HOST_STUB)
    # This row drives the RSX; nothing meaningful compiles against host
    # stubs, and the VP embed step needs the PS3 toolchain anyway.
    message(STATUS "shader-differential: skipped under PS3TC_REGRESSION_HOST_STUB")
    return()
endif()

include(ps3-self)

add_executable(shader-differential source/main.c)
target_compile_features(shader-differential PRIVATE c_std_11)

target_link_libraries(shader-differential PRIVATE
    rsx gcm_cmd gcm_sys io sysutil rt lv2 m
)

# The one embedded shader: the shared VP, which is NOT under test.
# Control/probe fragment shaders in shaders/ are compiled by the
# STAGER, not here — they must travel the same runtime-load path as
# every corpus pair, or the controls would not exercise the pair path.
ps3_add_cg_shader_rsxcgc(shader-differential shaders/sd_pos_uv.vcg)

ps3_add_self(shader-differential)
