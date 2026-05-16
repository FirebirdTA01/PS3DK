# PS3 Custom Toolchain — CMake toolchain file for the PPU LP64
# (-mlp64) multilib variant.
#
# Use with:
#   cmake -S <sample-dir> -B build \
#         -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain-lp64.cmake
#   cmake --build build
#
# Modeled on cmake/ps3-ppu-toolchain.cmake (ILP32 default).  Adds
# -mlp64 to all compiler / linker invocations so the GCC driver
# selects the lp64/ multilib variants of startfiles (lv2-crt0.o,
# lv2-crti.o), intrinsics (libgcc.a), and search paths (lib/lp64/).
#
# The sample CMakeLists.txt files do NOT need modification — this
# file overrides the compile/link flags globally so the same sources
# build as LP64.  Samples that create both ILP32 and LP64 targets
# (e.g., the mlp64-types sample) must use the ILP32 toolchain file
# for the ILP32 target and this file for the LP64 target; their
# CMakeLists already pass -mlp64 per-target for the LP64 target only.

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR powerpc64)
set(CMAKE_CROSSCOMPILING   TRUE)

# -----------------------------------------------------------------------------
# Locate $PS3DEV + $PS3DK
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

set(PS3_PPU_TARGET "powerpc64-ps3-elf")

# -----------------------------------------------------------------------------
# Toolchain executables — same as ILP32
# -----------------------------------------------------------------------------
set(_ps3_exe "")
if(CMAKE_HOST_WIN32)
    set(_ps3_exe ".exe")
endif()

set(CMAKE_C_COMPILER   "${PS3DEV}/ppu/bin/${PS3_PPU_TARGET}-gcc${_ps3_exe}"     CACHE FILEPATH "")
set(CMAKE_CXX_COMPILER "${PS3DEV}/ppu/bin/${PS3_PPU_TARGET}-g++${_ps3_exe}"     CACHE FILEPATH "")
set(CMAKE_ASM_COMPILER "${PS3DEV}/ppu/bin/${PS3_PPU_TARGET}-gcc${_ps3_exe}"     CACHE FILEPATH "")
set(CMAKE_AR           "${PS3DEV}/ppu/bin/${PS3_PPU_TARGET}-ar${_ps3_exe}"      CACHE FILEPATH "")
set(CMAKE_RANLIB       "${PS3DEV}/ppu/bin/${PS3_PPU_TARGET}-ranlib${_ps3_exe}"  CACHE FILEPATH "")
set(CMAKE_STRIP        "${PS3DEV}/ppu/bin/${PS3_PPU_TARGET}-strip${_ps3_exe}"   CACHE FILEPATH "")
set(CMAKE_OBJCOPY      "${PS3DEV}/ppu/bin/${PS3_PPU_TARGET}-objcopy${_ps3_exe}" CACHE FILEPATH "")
set(CMAKE_OBJDUMP      "${PS3DEV}/ppu/bin/${PS3_PPU_TARGET}-objdump${_ps3_exe}" CACHE FILEPATH "")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# -----------------------------------------------------------------------------
# Search-path policy — same as ILP32 (GCC -mlp64 handles multilib)
# -----------------------------------------------------------------------------
set(CMAKE_FIND_ROOT_PATH "${PS3DEV}/ppu" "${PS3DK}/ppu")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# -----------------------------------------------------------------------------
# LP64 flags — -mlp64 added to the ILP32 machdep baseline
# -----------------------------------------------------------------------------
# GCC's -mlp64 driver switch:
#   - Selects the lp64/ multilib (startfiles, libgcc, search paths)
#   - Sets __LP64__=1, __ILP32__=0
#   - Changes sizeof(long)=8, sizeof(void*)=8, sizeof(size_t)=8
set(_ps3_ppu_machdep "-mcpu=cell -mhard-float -fmodulo-sched -ffunction-sections -fdata-sections -mlp64")
set(CMAKE_C_FLAGS_INIT   "${_ps3_ppu_machdep}")
set(CMAKE_CXX_FLAGS_INIT "${_ps3_ppu_machdep}")
set(CMAKE_ASM_FLAGS_INIT "-mcpu=cell -mlp64")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_ps3_ppu_machdep}")

# Per-config defaults (same as ILP32)
set(CMAKE_C_FLAGS_RELEASE_INIT   "-O2 -Wall")
set(CMAKE_CXX_FLAGS_RELEASE_INIT "-O2 -Wall")
set(CMAKE_C_FLAGS_DEBUG_INIT     "-O0 -g -Wall")
set(CMAKE_CXX_FLAGS_DEBUG_INIT   "-O0 -g -Wall")

# -----------------------------------------------------------------------------
# SDK include paths — same as ILP32
# -----------------------------------------------------------------------------
include_directories(SYSTEM "${PS3DK}/ppu/include")
if(IS_DIRECTORY "${PS3DK}/ppu/include/simdmath")
    include_directories(SYSTEM "${PS3DK}/ppu/include/simdmath")
endif()

if(NOT DEFINED CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17 CACHE STRING "C++ standard")
endif()

# -----------------------------------------------------------------------------
# Explicit -lrt before the driver's auto-injected group
# (same as ILP32 — globfile.o constructor ordering)
# -----------------------------------------------------------------------------
link_libraries(rt -L${PS3DK}/ppu/lib/lp64)

# -----------------------------------------------------------------------------
# Make ps3-self.cmake findable via include() from sample CMakeLists
# -----------------------------------------------------------------------------
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")
