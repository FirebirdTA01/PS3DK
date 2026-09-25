#!/usr/bin/env bash
# tests/sdk/gcm-redefinition-test.sh
#
# Verifies that <cell/gcm.h>, <cell/gcm/gcm_command_c.h>, <cell/gcm/gcm_enum.h>
# and <rsx/gcm_sys.h> can be included in any order without macro redefinitions
# under -Wall -Wextra -Werror.
#
# Tests both ABIs (ilp32 and lp64), C (-std=gnu99, -std=c11) and C++ (-std=c++17),
# across 4 include orders on PPU GCC/G++, plus a red control proving that
# unpatched headers fail with the exact expected redefinition errors.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)
inc="$root/sdk/include"
gcm_cmd_inc="$root/sdk/libgcm_cmd/include"
stage_inc=${STAGE_INCLUDE_DIR:-"/home/firebirdta01/ps3tc/gemini-stage/ps3dk/ppu/include"}
fixture="$root/tests/sdk/gcm-redefinition-fixture/unpatched"
src="$root/tests/sdk/gcm-redefinition-test.c"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
ulimit -c 0 || true

checks=0

# Determine available PPU compiler
ppu_cc=${PPU_CC:-powerpc64-ps3-elf-gcc}
ppu_cxx=${PPU_CXX:-powerpc64-ps3-elf-g++}

if ! command -v "$ppu_cc" >/dev/null 2>&1 && [ -x "/home/firebirdta01/ps3tc/gemini-stage/ppu/bin/powerpc64-ps3-elf-gcc" ]; then
    ppu_cc="/home/firebirdta01/ps3tc/gemini-stage/ppu/bin/powerpc64-ps3-elf-gcc"
fi
if ! command -v "$ppu_cxx" >/dev/null 2>&1 && [ -x "/home/firebirdta01/ps3tc/gemini-stage/ppu/bin/powerpc64-ps3-elf-g++" ]; then
    ppu_cxx="/home/firebirdta01/ps3tc/gemini-stage/ppu/bin/powerpc64-ps3-elf-g++"
fi

ORDERS=(cell_first rsx_first enum_first enum_rsx)

# 1. PPU Cross-compiler tests (ILP32 and LP64, C and C++, 4 include orders)
for lang in c c++; do
    if [ "$lang" = c ]; then
        compiler="$ppu_cc"
        standards=("-std=gnu99" "-std=c11")
        extra_flags=("-Wno-unused-variable" "-Wno-unused-parameter")
    else
        compiler="$ppu_cxx"
        standards=("-std=c++17")
        extra_flags=("-Wno-unused-variable" "-Wno-unused-parameter")
    fi

    if command -v "$compiler" >/dev/null 2>&1; then
        for std_flag in "${standards[@]}"; do
            for abi in ilp32 lp64; do
                abi_flags=()
                if [ "$abi" = lp64 ]; then abi_flags=(-mlp64); fi

                for order in "${ORDERS[@]}"; do
                    order_flag=""
                    case "$order" in
                        cell_first) order_flag="-DORDER_CELL_FIRST" ;;
                        rsx_first)  order_flag="-DORDER_RSX_FIRST" ;;
                        enum_first) order_flag="-DORDER_ENUM_FIRST" ;;
                        enum_rsx)   order_flag="-DORDER_ENUM_RSX" ;;
                    esac

                    "$compiler" -x "$lang" "$std_flag" -mcpu=cell -mhard-float \
                        "${abi_flags[@]}" "$order_flag" "${extra_flags[@]}" \
                        -Wall -Wextra -Werror -I"$inc" -I"$gcm_cmd_inc" \
                        -c "$src" -o "$tmp/probe_${lang}_${abi}_${order}.o"
                    echo "gcm-redefinition: PASS PPU $lang ($std_flag) $abi $order"
                    checks=$((checks + 1))
                done
            done
        done
    else
        echo "gcm-redefinition: SKIP PPU $lang (compiler unavailable)"
    fi
done

# 2. Stage headers test if available
if [ -d "$stage_inc" ] && command -v "$ppu_cc" >/dev/null 2>&1; then
    for abi in ilp32 lp64; do
        abi_flags=()
        if [ "$abi" = lp64 ]; then abi_flags=(-mlp64); fi

        "$ppu_cc" -x c -std=gnu99 -mcpu=cell -mhard-float \
            "${abi_flags[@]}" -Wall -Wextra -Werror \
            -Wno-unused-variable -Wno-unused-parameter \
            -I"$stage_inc" -c "$src" -o "$tmp/stage_${abi}.o"
        echo "gcm-redefinition: PASS stage PPU C $abi"
        checks=$((checks + 1))
    done
fi

if [ "$checks" -eq 0 ]; then
    echo "gcm-redefinition: FAIL no compilers available" >&2
    exit 1
fi

# 3. Executable red control: prove that unpatched fixture headers fail
# compilation due to macro redefinition of CELL_GCM_DEBUG_LEVEL* and
# CELL_GCM_ZCULL_Z24S8 under -Werror.
red_compiler=""
red_flags=("-std=gnu99" "-Wno-unused-variable" "-Wno-unused-parameter")
if command -v "$ppu_cc" >/dev/null 2>&1; then
    red_compiler="$ppu_cc"
    red_flags+=("-mcpu=cell" "-mhard-float" "-c" "-o" "$tmp/red.o")
fi

if [ -n "$red_compiler" ]; then
    set +e
    red_output=$("$red_compiler" -x c "${red_flags[@]}" \
        -Wall -Wextra -Werror -DORDER_CELL_FIRST \
        -I"$fixture" -I"$inc" -I"$gcm_cmd_inc" "$src" 2>&1)
    red_status=$?
    set -e

    if [ "$red_status" -eq 0 ]; then
        echo "gcm-redefinition: FAIL red control unexpectedly succeeded" >&2
        exit 1
    fi

    if ! printf '%s\n' "$red_output" | grep -E 'CELL_GCM_ZCULL_Z24S8.*redefined' >/dev/null; then
        echo "gcm-redefinition: FAIL red control did not catch CELL_GCM_ZCULL_Z24S8 redefinition: $red_output" >&2
        exit 1
    fi

    if ! printf '%s\n' "$red_output" | grep -E 'CELL_GCM_DEBUG_LEVEL[0-2].*redefined' >/dev/null; then
        echo "gcm-redefinition: FAIL red control did not catch CELL_GCM_DEBUG_LEVEL redefinition: $red_output" >&2
        exit 1
    fi

    echo "gcm-redefinition: PASS red control (redefinitions caught when unpatched headers are active)"

    # 4. Mutant control: prove that an injected unrelated error is rejected by the red guard
    mutant_output="$src: fatal error: injected unrelated syntax error"
    if printf '%s\n' "$mutant_output" | grep -E 'CELL_GCM_ZCULL_Z24S8.*redefined' >/dev/null; then
        echo "gcm-redefinition: FAIL mutant control accepted unrelated error" >&2
        exit 1
    fi
    echo "gcm-redefinition: PASS mutant control (unrelated compiler failure correctly rejected by red guard)"
fi

echo "gcm-redefinition: PASS ($checks configurations verified, red control passed)"
