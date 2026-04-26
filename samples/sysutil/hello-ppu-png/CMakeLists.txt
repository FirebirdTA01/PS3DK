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

# Mirror sdk/assets/ICON0.PNG into pkg_files/USRDIR/ps3dk.png so /app_home
# resolves to it when the SELF is direct-booted, and so any future
# pkg-build step that copies pkg_files/* picks it up automatically.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../sdk/assets/ICON0.PNG"
    "${CMAKE_CURRENT_SOURCE_DIR}/pkg_files/USRDIR/ps3dk.png"
    COPYONLY)

ps3_add_self(hello-ppu-png)
