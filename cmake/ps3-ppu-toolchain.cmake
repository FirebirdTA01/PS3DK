# PS3 Custom Toolchain — CMake toolchain file for the PPU
# (powerpc64-ps3-elf).
#
# Use with:
#   cmake -S <sample-dir> -B build \
#         -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build
#
# Reads $PS3DEV / $PS3DK from the environment (set by sourcing
# scripts/env.sh on Linux or running setup.cmd on Windows).  The PPU
# GCC driver also reads $PS3DK at link time via getenv() in its
# LIB_LV2_SPEC, so -L resolves automatically against $PS3DK/ppu/lib.
#
# Helper module cmake/ps3-self.cmake provides ps3_add_self(target),
# which wires the strip / sprxlinker / make_self / fself post-build
# chain.  Sample CMakeLists pull it in via `include(ps3-self)`.

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
# Toolchain executables
# -----------------------------------------------------------------------------
# Add .exe suffix on Windows hosts where the prebuilt release lives.
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

# Try-compile uses STATIC_LIBRARY mode so CMake doesn't try to link a
# default executable (which would need our LV2 startfiles already).
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# -----------------------------------------------------------------------------
# Search-path policy
# -----------------------------------------------------------------------------
# Header / library lookups should hit $PS3DEV/ppu and $PS3DK/ppu only —
# the host's own /usr/include is irrelevant for a cross-target.  We
# explicitly INCLUDE programs from default paths because the build
# host's tools (cmake, ninja, python) need to be found.
set(CMAKE_FIND_ROOT_PATH "${PS3DEV}/ppu" "${PS3DK}/ppu")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# -----------------------------------------------------------------------------
# Default flags — match samples/.../Makefile + PSL1GHT's MACHDEP
# -----------------------------------------------------------------------------
# -mcpu=cell selects the Cell PPE; -mhard-float / -fmodulo-sched are
# the same flags ppu_rules emits via $(MACHDEP).  -ffunction-sections /
# -fdata-sections match ppu_rules so each function/object lands in its
# own section — but we deliberately do NOT pass --gc-sections to the
# linker.  PSL1GHT's ppu_rules doesn't either, and for good reason: GCM
# samples place command buffers / vertex buffers / TLS scratch in BSS
# that the kernel callback path (registered via cellGcmSetFlipHandler
# etc., reaching .opd through 32-bit EAs) reaches without a static
# reference the linker can trace.  --gc-sections silently strips them
# — observed symptom is a black-screen + emulator crash on exit
# (hello-ppu-cellgcm-cube .bss collapsed from ~256 KiB to ~1 KiB).
set(_ps3_ppu_machdep "-mcpu=cell -mhard-float -fmodulo-sched -ffunction-sections -fdata-sections")
set(CMAKE_C_FLAGS_INIT   "${_ps3_ppu_machdep}")
set(CMAKE_CXX_FLAGS_INIT "${_ps3_ppu_machdep}")
set(CMAKE_ASM_FLAGS_INIT "-mcpu=cell")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_ps3_ppu_machdep}")

# Per-config defaults.  Release-with-warnings matches the
# samples/.../Makefile's CFLAGS = -O2 -Wall.
set(CMAKE_C_FLAGS_RELEASE_INIT   "-O2 -Wall")
set(CMAKE_CXX_FLAGS_RELEASE_INIT "-O2 -Wall")
set(CMAKE_C_FLAGS_DEBUG_INIT     "-O0 -g -Wall")
set(CMAKE_CXX_FLAGS_DEBUG_INIT   "-O0 -g -Wall")

# -----------------------------------------------------------------------------
# SDK include paths
# -----------------------------------------------------------------------------
# $PS3DK/ppu/include holds our cell-SDK-style headers (cell/*.h, sys/*.h)
# and the simdmath subtree.  The PPU GCC driver doesn't auto-add this;
# samples need it for `#include <cell/gcm.h>` etc.
include_directories(SYSTEM "${PS3DK}/ppu/include")
if(IS_DIRECTORY "${PS3DK}/ppu/include/simdmath")
    include_directories(SYSTEM "${PS3DK}/ppu/include/simdmath")
endif()

# Default to C++17.  Individual targets override via target_compile_features
# or a per-target CMAKE_CXX_STANDARD.
if(NOT DEFINED CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17 CACHE STRING "C++ standard")
endif()

# -----------------------------------------------------------------------------
# Make ps3-self.cmake findable via include() from sample CMakeLists
# -----------------------------------------------------------------------------
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")
