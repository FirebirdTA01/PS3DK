# hello-ppu-cellgcm-vp-loop — VP loop-branch FIFO desync repro.
#
# Builds with a 10-instruction VP shader (>7 insns), forcing
# libgcm_cmd's LoadVertexProgramBlock to take the loop branch and
# emit a count=32 method packet. If RPCS3 desyncs on count=32 it'll
# show up here.
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-cellgcm-vp-loop C ASM)

include(ps3-self)

add_executable(hello-ppu-cellgcm-vp-loop source/main.c)
target_compile_features(hello-ppu-cellgcm-vp-loop PRIVATE c_std_11)

target_link_libraries(hello-ppu-cellgcm-vp-loop PRIVATE
    rsx gcm_cmd gcm_sys io sysutil sysmodule rt lv2 m
)

# Still on cgcomp.  rsx-cg-compiler now handles chained mul() for the
# matrix-only case (mul_chain4_passthrough_v in tests/shaders byte-
# matches sce-cgc).  vp-loop adds per-channel colour arithmetic
# (`c.r = c.r * tweak.x + tweak.y + offset.x; ...`) that needs MAD
# fusion + interleaved emit ordering — captured in
# tests/shaders/mul_chain4_v.cg as the failing fixture.  Migrate once
# the MAD fusion path lands.  See samples/gcm/hello-ppu-cellgcm-cube
# for the canonical migration template.
ps3_add_cg_shader(hello-ppu-cellgcm-vp-loop shaders/vpshader.vcg)
ps3_add_cg_shader(hello-ppu-cellgcm-vp-loop shaders/fpshader.fcg)

ps3_add_self(hello-ppu-cellgcm-vp-loop)
