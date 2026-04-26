#!/usr/bin/env bash
# PS3 Custom Toolchain — environment
#
# Source this file to activate the toolchain:
#   source ./scripts/env.sh
#
# Exports:
#   PS3_TOOLCHAIN_ROOT   repo root (absolute)
#   PS3DEV               install prefix ($PS3_TOOLCHAIN_ROOT/stage/ps3dev)
#   PPU_PREFIX           $PS3DEV/ppu          (newlib sysroot for PPU)
#   SPU_PREFIX           $PS3DEV/spu          (newlib sysroot for SPU)
#   PS3DK                $PS3DEV/ps3dk        (unified runtime + SDK install
#                                               root — libsysbase, libc, librt,
#                                               liblv2, libgcm_cmd, libdbgfont,
#                                               cell/* + sys/* headers, and the
#                                               ppu_rules / spu_rules / base_rules
#                                               Makefile fragments all land here.
#                                               Read by the PPU GCC driver via
#                                               getenv() in LIB_LV2_SPEC.)
#   PSL1GHT              $PS3DK               (back-compat alias for sample
#                                               Makefiles that still include
#                                               $(PSL1GHT)/ppu_rules; same path
#                                               as PS3DK now that build-psl1ght.sh
#                                               installs into the unified tree)
#   PS3_BUILD_ROOT       short path used for intermediate builds to avoid MAX_PATH
#
# Adds $PS3DEV/bin, $PPU_PREFIX/bin, and $SPU_PREFIX/bin to PATH.

# Resolve the repo root from this script's location.
_env_self="${BASH_SOURCE[0]}"
if [[ -z "$_env_self" ]]; then
    echo "env.sh must be sourced from bash, not executed directly." >&2
    return 1 2>/dev/null || exit 1
fi
_env_dir="$(cd "$(dirname "$_env_self")" && pwd -P)"
export PS3_TOOLCHAIN_ROOT="$(cd "$_env_dir/.." && pwd -P)"

export PS3DEV="${PS3DEV:-$PS3_TOOLCHAIN_ROOT/stage/ps3dev}"
export PPU_PREFIX="$PS3DEV/ppu"
export SPU_PREFIX="$PS3DEV/spu"
export PS3DK="$PS3DEV/ps3dk"
# PSL1GHT is now a back-compat alias for PS3DK — sample Makefiles
# include $(PSL1GHT)/ppu_rules, and build-psl1ght.sh installs the
# runtime + Makefile rules into PS3DK.
export PSL1GHT="$PS3DK"

# Default pkg icon for every sample that doesn't set ICON0 itself.
# PSL1GHT's ppu_rules does `ICON0 ?= $(PS3DEV)/bin/ICON0.PNG` - exporting
# ICON0 here wins the ?= and makes the pkg pipeline read directly from
# sdk/assets/ instead of the staged copy.  Editing sdk/assets/ICON0.PNG
# then immediately propagates to every sample's next `make pkg`, no
# build-sdk.sh step required.
export ICON0="$PS3_TOOLCHAIN_ROOT/sdk/assets/ICON0.PNG"

# Short build root avoids MAX_PATH blow-ups when GCC nests autotools output.
# Override in your shell if C:/ps3tc is unsuitable.
export PS3_BUILD_ROOT="${PS3_BUILD_ROOT:-$HOME/ps3tc/build}"

# Prepend toolchain bin dirs so powerpc64-ps3-elf-* / spu-elf-* / ppu-* /
# spu-* shadow any system copies.  Target-specific prefixes first (that's
# where GCC/binutils actually install), then the umbrella $PS3DEV/bin which
# is a future home for unified symlinks / PSL1GHT helpers.
for _p in "$PS3DEV/bin" "$PPU_PREFIX/bin" "$SPU_PREFIX/bin"; do
    case ":$PATH:" in
        *":$_p:"*) ;;
        *) export PATH="$_p:$PATH" ;;
    esac
done
unset _p

# MSYS2 safety net: if we're in MSYS2, make sure the mingw64 toolchain comes first
# (mingw's gcc is the host compiler used to build the cross toolchain).
if [[ "${MSYSTEM:-}" == "MINGW64" || "${MSYSTEM:-}" == "UCRT64" ]]; then
    case ":$PATH:" in
        *":/mingw64/bin:"*) ;;
        *) export PATH="/mingw64/bin:$PATH" ;;
    esac
fi

# Informational banner only when sourced interactively.
if [[ $- == *i* ]]; then
    echo "ps3 toolchain env loaded"
    echo "  PS3_TOOLCHAIN_ROOT = $PS3_TOOLCHAIN_ROOT"
    echo "  PS3DEV             = $PS3DEV"
    echo "  PS3_BUILD_ROOT     = $PS3_BUILD_ROOT"
fi
