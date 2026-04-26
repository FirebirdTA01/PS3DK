# PS3 Custom Toolchain — CMake toolchain file for the SPU (spu-elf).
#
# Use with:
#   cmake -S <sample-dir> -B build \
#         -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-spu-toolchain.cmake
#   cmake --build build
#
# SPU code targets the Synergistic Processing Element (SPE) cores.
# Each SPU has 256 KB local store, no MMU, and no exceptions / RTTI;
# the default flags reflect that — code-size first.
#
# For PPU+SPU integration samples (PPU spawns an SPU thread and
# embeds an SPU image at link time), follow the embedspu pattern:
# build the SPU side with this toolchain, then have the PPU build
# call `embedspu` on the resulting .elf to produce a PPU-linkable
# .o.  See the integration-sample CMakeLists in 7c (deferred).

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR spu)
set(CMAKE_CROSSCOMPILING   TRUE)

# -----------------------------------------------------------------------------
# Locate $PS3DEV
# -----------------------------------------------------------------------------
if(NOT DEFINED ENV{PS3DEV} AND NOT DEFINED PS3DEV)
    message(FATAL_ERROR
        "PS3DEV is not set in the environment.\n"
        "  Linux:   source scripts/env.sh\n"
        "  Windows: cd %PS3DK% && setup.cmd")
endif()
set(PS3DEV "$ENV{PS3DEV}" CACHE PATH "PS3 toolchain install root")

if(DEFINED ENV{PS3DK})
    set(PS3DK "$ENV{PS3DK}" CACHE PATH "PS3 SDK runtime root (umbrella)")
else()
    set(PS3DK "${PS3DEV}/ps3dk" CACHE PATH "PS3 SDK runtime root (umbrella)")
endif()

set(PS3_SPU_TARGET "spu-elf")

# -----------------------------------------------------------------------------
# Toolchain executables
# -----------------------------------------------------------------------------
set(_ps3_exe "")
if(CMAKE_HOST_WIN32)
    set(_ps3_exe ".exe")
endif()

set(CMAKE_C_COMPILER   "${PS3DEV}/spu/bin/${PS3_SPU_TARGET}-gcc${_ps3_exe}"     CACHE FILEPATH "")
set(CMAKE_CXX_COMPILER "${PS3DEV}/spu/bin/${PS3_SPU_TARGET}-g++${_ps3_exe}"     CACHE FILEPATH "")
set(CMAKE_ASM_COMPILER "${PS3DEV}/spu/bin/${PS3_SPU_TARGET}-gcc${_ps3_exe}"     CACHE FILEPATH "")
set(CMAKE_AR           "${PS3DEV}/spu/bin/${PS3_SPU_TARGET}-ar${_ps3_exe}"      CACHE FILEPATH "")
set(CMAKE_RANLIB       "${PS3DEV}/spu/bin/${PS3_SPU_TARGET}-ranlib${_ps3_exe}"  CACHE FILEPATH "")
set(CMAKE_STRIP        "${PS3DEV}/spu/bin/${PS3_SPU_TARGET}-strip${_ps3_exe}"   CACHE FILEPATH "")
set(CMAKE_OBJCOPY      "${PS3DEV}/spu/bin/${PS3_SPU_TARGET}-objcopy${_ps3_exe}" CACHE FILEPATH "")
set(CMAKE_OBJDUMP      "${PS3DEV}/spu/bin/${PS3_SPU_TARGET}-objdump${_ps3_exe}" CACHE FILEPATH "")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# -----------------------------------------------------------------------------
# Search-path policy
# -----------------------------------------------------------------------------
set(CMAKE_FIND_ROOT_PATH "${PS3DEV}/spu" "${PS3DK}/spu")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# -----------------------------------------------------------------------------
# Default flags — code-size + no exceptions / RTTI
# -----------------------------------------------------------------------------
# -Os because each SPU has 256 KB LS to spare.  No exceptions / no RTTI
# matches the GCC 9.5 SPU spec file's default: libstdc++ on SPU was
# built --disable-libstdcxx-exception-handling for the same reason.
# -ffunction-sections / -fdata-sections + -Wl,--gc-sections keep
# unused code out of the LS image.
set(_ps3_spu_baseflags "-Os -ffunction-sections -fdata-sections")
set(_ps3_spu_cxx_baseflags "${_ps3_spu_baseflags} -fno-exceptions -fno-rtti")
set(CMAKE_C_FLAGS_INIT   "${_ps3_spu_baseflags}")
set(CMAKE_CXX_FLAGS_INIT "${_ps3_spu_cxx_baseflags}")
set(CMAKE_ASM_FLAGS_INIT "")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_ps3_spu_baseflags} -Wl,--gc-sections")

set(CMAKE_C_FLAGS_RELEASE_INIT   "-Os -Wall")
set(CMAKE_CXX_FLAGS_RELEASE_INIT "-Os -Wall")
set(CMAKE_C_FLAGS_DEBUG_INIT     "-Og -g -Wall")
set(CMAKE_CXX_FLAGS_DEBUG_INIT   "-Og -g -Wall")

# -----------------------------------------------------------------------------
# SDK include paths
# -----------------------------------------------------------------------------
if(IS_DIRECTORY "${PS3DK}/spu/include")
    include_directories(SYSTEM "${PS3DK}/spu/include")
endif()

# Default to C++17 — GCC 9.5 has a complete C++17 implementation.
# C++20 / C++23 require the SPU backend forward-port (separate phase).
if(NOT DEFINED CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17 CACHE STRING "C++ standard")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")
