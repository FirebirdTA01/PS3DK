cmake_minimum_required(VERSION 3.20)
project(hello-psgl-rotating-quad C ASM)

include(ps3-self)

add_executable(hello-psgl-rotating-quad source/main.c)
target_compile_features(hello-psgl-rotating-quad PRIVATE c_std_11)
target_link_libraries(hello-psgl-rotating-quad PRIVATE
    PSGL
    PSGLU
    cgb
    pngdec_stub
    sysmodule_stub
    fs_stub
    gcm_cmd
    gcm_sys
    rsx
    sysutil_stub
    sysutil
    m)

set(_icon0_src "${CMAKE_CURRENT_SOURCE_DIR}/../../../sdk/assets/ICON0.PNG")
set(_icon0_dst "${CMAKE_CURRENT_BINARY_DIR}/ICON0.PNG")
configure_file("${_icon0_src}" "${_icon0_dst}" COPYONLY)
ps3_bin2s(hello-psgl-rotating-quad "${_icon0_dst}")
ps3_add_self(hello-psgl-rotating-quad)
