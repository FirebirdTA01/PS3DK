cmake_minimum_required(VERSION 3.20)
project(hello-ppu-cellgcm-blend C ASM)

include(ps3-self)

add_executable(hello-ppu-cellgcm-blend source/main.c)
target_compile_features(hello-ppu-cellgcm-blend PRIVATE c_std_11)

target_link_libraries(hello-ppu-cellgcm-blend PRIVATE
    gcm_cmd gcm_sys io sysutil sysmodule rt lv2 m
)

ps3_add_cg_shader_rsxcgc(hello-ppu-cellgcm-blend shaders/vpshader.vcg)
ps3_add_cg_shader_rsxcgc(hello-ppu-cellgcm-blend shaders/fpshader.fcg)

# Legacy cgcomp path — see samples/gcm/hello-ppu-cellgcm-triangle for
# the canonical migration note.  cgcomp + cg.dll are scheduled for
# removal; the rsx*Program loader API in main.c is *not*.
#ps3_add_cg_shader(hello-ppu-cellgcm-blend shaders/vpshader.vcg)
#ps3_add_cg_shader(hello-ppu-cellgcm-blend shaders/fpshader.fcg)

ps3_add_self(hello-ppu-cellgcm-blend)
