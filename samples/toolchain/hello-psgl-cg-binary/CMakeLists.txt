cmake_minimum_required(VERSION 3.20)
project(hello-psgl-cg-binary C ASM)

include(ps3-self)

add_executable(hello-psgl-cg-binary source/main.c)
target_compile_features(hello-psgl-cg-binary PRIVATE c_std_11)
target_link_libraries(hello-psgl-cg-binary PRIVATE
    PSGL
    PSGLU
    cgb
    fs_stub
    gcm_cmd
    gcm_sys
    rsx
    sysutil_stub
    sysutil
    m)

# rsx-cg-compiler writes the raw .vpo/.fpo program containers; bin2s embeds
# them as tintvp_vpo / tintfp_fpo.
ps3_add_cg_shader_rsxcgc(hello-psgl-cg-binary shaders/tintvp.vcg)
ps3_add_cg_shader_rsxcgc(hello-psgl-cg-binary shaders/tintfp.fcg)
ps3_add_self(hello-psgl-cg-binary)
