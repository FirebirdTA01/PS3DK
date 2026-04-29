# hello-ppu-cellgcm-triangle — CMake build.
#
# Compiles vpshader.vcg / fpshader.fcg via rsx-cg-compiler (CgBinaryProgram
# container output), embeds them via bin2s, and links against the cellGcm
# forwarders + RSX runtime.
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build
# Produces:  build/hello-ppu-cellgcm-triangle{,.stripped.elf,.self,.fake.self}

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-cellgcm-triangle C ASM)

include(ps3-self)

add_executable(hello-ppu-cellgcm-triangle source/main.c)
target_compile_features(hello-ppu-cellgcm-triangle PRIVATE c_std_11)

target_link_libraries(hello-ppu-cellgcm-triangle PRIVATE
    gcm_cmd gcm_sys io sysutil sysmodule rt lv2 m
)

ps3_add_cg_shader_rsxcgc(hello-ppu-cellgcm-triangle shaders/vpshader.vcg)
ps3_add_cg_shader_rsxcgc(hello-ppu-cellgcm-triangle shaders/fpshader.fcg)

# Legacy cgcomp path — kept commented for reference.  cgcomp + cg.dll are
# scheduled for removal once rsx-cg-compiler covers all sample shader
# features (see samples/README.md).  The rsx* loader API in main.c is
# *not* going away with cgcomp — it'll be reskinned to consume the same
# CgBinaryProgram blobs rsx-cg-compiler emits, so legacy homebrew
# rebuilds without source changes.
#ps3_add_cg_shader(hello-ppu-cellgcm-triangle shaders/vpshader.vcg)
#ps3_add_cg_shader(hello-ppu-cellgcm-triangle shaders/fpshader.fcg)

ps3_add_self(hello-ppu-cellgcm-triangle)
