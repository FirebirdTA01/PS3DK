cmake_minimum_required(VERSION 3.20)
project(hello-psgl-ffp-lit-bootstrap C ASM)

include(ps3-self)

add_executable(hello-psgl-ffp-lit-bootstrap source/main.c)
target_compile_features(hello-psgl-ffp-lit-bootstrap PRIVATE c_std_11)
target_link_libraries(hello-psgl-ffp-lit-bootstrap PRIVATE
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

ps3_add_self(hello-psgl-ffp-lit-bootstrap)
