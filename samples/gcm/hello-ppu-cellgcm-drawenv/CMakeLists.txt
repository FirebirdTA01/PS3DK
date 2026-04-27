# hello-ppu-cellgcm-drawenv — replays the EMP engine's setSurface +
# setDrawEnv sequence on top of the canonical clear loop, to determine
# whether the FIFO desync the engine hits with setDrawEnv enabled is
# reproducible from toolchain code or only from engine-side sequencing.
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build

cmake_minimum_required(VERSION 3.20)
project(hello-ppu-cellgcm-drawenv C)

include(ps3-self)

add_executable(hello-ppu-cellgcm-drawenv source/main.c)
target_compile_features(hello-ppu-cellgcm-drawenv PRIVATE c_std_11)

target_link_libraries(hello-ppu-cellgcm-drawenv PRIVATE
    rsx gcm_cmd gcm_sys io sysutil rt lv2
)

ps3_add_self(hello-ppu-cellgcm-drawenv)
