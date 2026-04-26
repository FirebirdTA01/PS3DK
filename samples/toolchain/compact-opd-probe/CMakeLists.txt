# compact-opd-probe — CMake build.
#
# Build:
#   cmake -S . -B build \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build
# Produces:  build/compact-opd-probe{,.stripped.elf,.self,.fake.self}

cmake_minimum_required(VERSION 3.20)
project(compact-opd-probe C ASM)

include(ps3-self)

# Includes a hand-written .S that defines `compact_target` for the
# probe to call — the whole point of the sample is exercising the
# compiler's compact-OPD descriptor at a known target symbol.
add_executable(compact-opd-probe
    source/main.c
    source/compact_target.S)
target_compile_features(compact-opd-probe PRIVATE c_std_11)

target_link_libraries(compact-opd-probe PRIVATE
    sysutil rt lv2 
)

ps3_add_self(compact-opd-probe)
