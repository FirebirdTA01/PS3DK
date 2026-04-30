# hello-ppu-cellgcm-global-uniforms — same rotating-cube renderer as
# hello-ppu-cellgcm-textured-cube, but the shaders use Cg's "implicit
# uniform" file-scope syntax (no `uniform` keyword) and a struct-
# return VP instead of `out`-parameter outputs.  Both samples ship
# side-by-side as the "with-keyword vs without-keyword" reference
# pair for the rsx-cg-compiler globals path.
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-cellgcm-global-uniforms C ASM)

include(ps3-self)

add_executable(hello-ppu-cellgcm-global-uniforms source/main.c)
target_compile_features(hello-ppu-cellgcm-global-uniforms PRIVATE c_std_11)

target_link_libraries(hello-ppu-cellgcm-global-uniforms PRIVATE
    pngdec_stub sysmodule_stub
    rsx gcm_cmd gcm_sys io sysutil rt lv2 m
)

ps3_add_cg_shader_rsxcgc(hello-ppu-cellgcm-global-uniforms shaders/vpshader.vcg)
ps3_add_cg_shader_rsxcgc(hello-ppu-cellgcm-global-uniforms shaders/fpshader.fcg)

# Embed the SDK's ICON0.PNG so the FP samples a real texture at runtime.
set(_icon0_src "${CMAKE_CURRENT_SOURCE_DIR}/../../../sdk/assets/ICON0.PNG")
set(_icon0_dst "${CMAKE_CURRENT_BINARY_DIR}/ICON0.PNG")
configure_file("${_icon0_src}" "${_icon0_dst}" COPYONLY)
ps3_bin2s(hello-ppu-cellgcm-global-uniforms "${_icon0_dst}")

ps3_add_self(hello-ppu-cellgcm-global-uniforms)

ps3_add_pkg(hello-ppu-cellgcm-global-uniforms
    TITLE     "PS3DK Global Uniforms"
    APPID     "GLBUNIF01"
    CONTENTID "UP0001-GLBUNIF01_00-PS3DKTOOLCHAIN00")
