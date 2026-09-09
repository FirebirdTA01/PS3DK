# hello-ppu-cellgcm-sysinfo — CMake build.
#
# Runtime coverage for the GCM system-query surface restored in issue #7.
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build
# Produces:  build/hello-ppu-cellgcm-sysinfo{,.stripped.elf,.self,.fake.self}

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-cellgcm-sysinfo C)

include(ps3-self)

add_executable(hello-ppu-cellgcm-sysinfo source/main.c)
target_compile_features(hello-ppu-cellgcm-sysinfo PRIVATE c_std_11)

target_link_libraries(hello-ppu-cellgcm-sysinfo PRIVATE
    rsx gcm_cmd gcm_sys io sysutil rt lv2
)

ps3_add_self(hello-ppu-cellgcm-sysinfo)

ps3_add_pkg(hello-ppu-cellgcm-sysinfo
    APPID     "GCMX00021"
    CONTENTID "UP0001-GCMX00021_00-PS3DKSAMPLES0000")
