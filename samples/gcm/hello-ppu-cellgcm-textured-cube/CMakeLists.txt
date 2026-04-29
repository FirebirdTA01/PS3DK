# hello-ppu-cellgcm-textured-cube — rotating cube with the SDK ICON0
# texture mapped onto each face.
#
# Combines hello-ppu-cellgcm-cube (perspective + rotation MVP, depth
# test, indexed draw) with hello-ppu-png's cellPngDec decode flow so
# the sample loads a real PNG (sdk/assets/ICON0.PNG) at startup,
# converts RGBA → ARGB into RSX-local memory, and samples it from the
# fragment program with the canonical texture-unit setup.
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-cellgcm-textured-cube C ASM)

include(ps3-self)

add_executable(hello-ppu-cellgcm-textured-cube source/main.c)
target_compile_features(hello-ppu-cellgcm-textured-cube PRIVATE c_std_11)

target_link_libraries(hello-ppu-cellgcm-textured-cube PRIVATE
    pngdec_stub sysmodule_stub
    rsx gcm_cmd gcm_sys io sysutil rt lv2 m
)

ps3_add_cg_shader_rsxcgc(hello-ppu-cellgcm-textured-cube shaders/vpshader.vcg)
ps3_add_cg_shader_rsxcgc(hello-ppu-cellgcm-textured-cube shaders/fpshader.fcg)

# Legacy cgcomp path — see samples/gcm/hello-ppu-cellgcm-triangle for
# the canonical migration note.
#ps3_add_cg_shader(hello-ppu-cellgcm-textured-cube shaders/vpshader.vcg)
#ps3_add_cg_shader(hello-ppu-cellgcm-textured-cube shaders/fpshader.fcg)

# Embed sdk/assets/ICON0.PNG as a binary blob the source can decode at
# startup.  bin2s names the symbols off the file's basename (dots
# replaced by underscores), so ICON0.PNG -> ICON0_PNG[] / ICON0_PNG_size.
set(_icon0_src "${CMAKE_CURRENT_SOURCE_DIR}/../../../sdk/assets/ICON0.PNG")
set(_icon0_dst "${CMAKE_CURRENT_BINARY_DIR}/ICON0.PNG")
configure_file("${_icon0_src}" "${_icon0_dst}" COPYONLY)
ps3_bin2s(hello-ppu-cellgcm-textured-cube "${_icon0_dst}")

ps3_add_self(hello-ppu-cellgcm-textured-cube)

ps3_add_pkg(hello-ppu-cellgcm-textured-cube
    TITLE     "PS3DK Textured Cube"
    APPID     "TEXCUBE01"
    CONTENTID "UP0001-TEXCUBE01_00-PS3DKTOOLCHAIN00")
