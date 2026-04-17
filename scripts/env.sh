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
#   PSL1GHT              $PS3DEV/psl1ght      (PSL1GHT install root; tools look here)
#   PS3_BUILD_ROOT       short path used for intermediate builds to avoid MAX_PATH
#
# Adds $PS3DEV/bin to PATH.

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
export PSL1GHT="$PS3DEV/psl1ght"

# Short build root avoids MAX_PATH blow-ups when GCC nests autotools output.
# Override in your shell if C:/ps3tc is unsuitable.
export PS3_BUILD_ROOT="${PS3_BUILD_ROOT:-$HOME/ps3tc/build}"

# Prepend $PS3DEV/bin so ppu-gcc/spu-gcc/etc. shadow any system copies.
case ":$PATH:" in
    *":$PS3DEV/bin:"*) ;;
    *) export PATH="$PS3DEV/bin:$PATH" ;;
esac

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
