# shader-readback — tier-c judged shader tests (regression battery row).
#
# Renders each test fragment program into an off-screen RT on RPCS3 and
# compares every pixel against PPU-computed expected values.  See
# docs/design/shader-compiler-testing.md §3 tier c.

cmake_minimum_required(VERSION 3.20)

project(shader_readback_regression C ASM)

option(PS3TC_REGRESSION_HOST_STUB "Build with host stubs for CI syntax/link coverage" OFF)

if(PS3TC_REGRESSION_HOST_STUB)
    # This row drives the RSX; there is nothing meaningful to compile
    # against host stubs, and the shader embed step needs the PS3
    # toolchain's compiler anyway.
    message(STATUS "shader-readback: skipped under PS3TC_REGRESSION_HOST_STUB")
    return()
endif()

include(ps3-self)

add_executable(shader-readback source/main.c)
target_compile_features(shader-readback PRIVATE c_std_11)

target_link_libraries(shader-readback PRIVATE
    rsx gcm_cmd gcm_sys io sysutil rt lv2 m
)

ps3_add_cg_shader_rsxcgc(shader-readback shaders/rb_pos_uv.vcg)
ps3_add_cg_shader_rsxcgc(shader-readback shaders/rb_solid.fcg)
ps3_add_cg_shader_rsxcgc(shader-readback shaders/rb_interp.fcg)
ps3_add_cg_shader_rsxcgc(shader-readback shaders/rb_arith.fcg)

ps3_add_self(shader-readback)
