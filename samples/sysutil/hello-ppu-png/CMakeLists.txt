# hello-ppu-png — CMake build.
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build
# Produces:  build/hello-ppu-png{,.stripped.elf,.self,.fake.self}

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-png C ASM)

include(ps3-self)

add_executable(hello-ppu-png source/main.c)
target_compile_features(hello-ppu-png PRIVATE c_std_11)

target_link_libraries(hello-ppu-png PRIVATE
    pngdec_stub sysmodule_stub gcm_sys rsx io sysutil rt lv2 m 
)

ps3_bin2s(hello-ppu-png data/test.png)

ps3_add_self(hello-ppu-png)
get_target_property(_png_output hello-ppu-png PS3_SELF_OUTPUT_DIRECTORY)

# Mirror sdk/assets/ICON0.PNG into pkg_files/USRDIR/ps3dk.png so /app_home
# resolves to it when the SELF is direct-booted from its selected directory,
# and so any future pkg-build step that copies pkg_files/* picks it up automatically.
# An explicit output override moves the asset with the SELF.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../sdk/assets/ICON0.PNG"
    "${_png_output}/pkg_files/USRDIR/ps3dk.png"
    COPYONLY)

ps3_add_pkg(hello-ppu-png
    APPID     "SYUT00023"
    CONTENTID "UP0001-SYUT00023_00-PS3DKSAMPLES0000"
    PKGFILES  "${_png_output}/pkg_files")
