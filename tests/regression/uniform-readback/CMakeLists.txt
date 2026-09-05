# uniform-readback — regression row asserting fragment uniform application.
#
# Renders two successive fullscreen quads to an off-screen render target,
# setting a different fragment uniform value before each draw. Reads the pixels
# back to main memory and asserts that the two draws produced DIFFERENT output.
#
# Failure condition: Sameness (draw1 == draw2).
# If cellGcmSetFragmentProgramParameter silently fails to patch RSX local memory,
# both draws render with the compile-time baked default constants (all 0s) and
# the test fails loudly with UNIFORM_READBACK_FAIL.

cmake_minimum_required(VERSION 3.20)
project(uniform_readback_regression C ASM)

option(PS3TC_REGRESSION_HOST_STUB "Build with host stubs for CI syntax/link coverage" OFF)

if(PS3TC_REGRESSION_HOST_STUB)
    message(STATUS "uniform-readback: skipped under PS3TC_REGRESSION_HOST_STUB")
    return()
endif()

include(ps3-self)

add_executable(uniform-readback source/main.c)
target_compile_features(uniform-readback PRIVATE c_std_11)

target_link_libraries(uniform-readback PRIVATE
    rsx gcm_cmd gcm_sys io sysutil rt lv2 m
)

ps3_add_cg_shader_rsxcgc(uniform-readback shaders/ru_pos_uv.vcg)
ps3_add_cg_shader_rsxcgc(uniform-readback shaders/ru_uniform.fcg)

ps3_add_self(uniform-readback)
