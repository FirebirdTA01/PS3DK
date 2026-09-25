#!/usr/bin/env bash
# sys-process-param-visibility-test.sh — Test matrix for SYS_PROCESS_PARAM visibility via cell/spurs.h and sys/dbg.h
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
SDK_INC="$ROOT_DIR/sdk/include"

PPU_CC="${PPU_CC:-powerpc64-ps3-elf-gcc}"
PPU_CXX="${PPU_CXX:-powerpc64-ps3-elf-g++}"

if ! command -v "$PPU_CC" >/dev/null 2>&1; then
    if [ -n "${PS3DEV:-}" ] && [ -x "$PS3DEV/ppu/bin/powerpc64-ps3-elf-gcc" ]; then
        PPU_CC="$PS3DEV/ppu/bin/powerpc64-ps3-elf-gcc"
        PPU_CXX="$PS3DEV/ppu/bin/powerpc64-ps3-elf-g++"
    else
        echo "sys-process-param-visibility: SKIP (PPU cross-compiler unavailable)"
        exit 0
    fi
fi

stage_inc=${PS3DK_INC:-${PS3DEV:+$PS3DEV/ps3dk/ppu/include}}
inc_flags=(-I"$SDK_INC" -D__PS3DK_SDK_SELFBUILD__)
if [ -n "${stage_inc:-}" ] && [ -d "$stage_inc" ]; then
    inc_flags+=(-isystem "$stage_inc")
fi

HEADERS=(
    "TEST_INCLUDE_CELL_SPURS"
    "TEST_INCLUDE_SYS_DBG"
)

C_MODES=("-std=c99" "-std=c11" "-std=gnu99" "-std=gnu11")
CXX_MODES=("-std=c++98" "-std=c++11" "-std=c++14" "-std=c++17")

tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT

checks=0

echo "=== sys-process-param-visibility-test: starting ==="

# 1. C test matrix across both ABIs
for hdr in "${HEADERS[@]}"; do
    for c_mode in "${C_MODES[@]}"; do
        for abi in "" "-mlp64"; do
            c_extra=()
            if [ -z "$abi" ]; then
                c_extra+=("-Wno-pointer-to-int-cast")
            fi
            $PPU_CC $abi $c_mode -Wall -Wextra -Werror "${c_extra[@]}" \
                "${inc_flags[@]}" \
                -D"$hdr"=1 \
                -c "$SCRIPT_DIR/sys-process-param-visibility-test.c" \
                -o "$tmp_dir/test_c.o"
            checks=$((checks + 1))
        done
    done
done
echo "sys-process-param-visibility: PASS candidate PPU C (${#HEADERS[@]} headers x ${#C_MODES[@]} modes x 2 ABIs)"

# 2. C++ test matrix across both ABIs
for hdr in "${HEADERS[@]}"; do
    for cxx_mode in "${CXX_MODES[@]}"; do
        for abi in "" "-mlp64"; do
            $PPU_CXX $abi $cxx_mode -Wall -Wextra -Werror \
                "${inc_flags[@]}" \
                -D"$hdr"=1 \
                -x c++ \
                -c "$SCRIPT_DIR/sys-process-param-visibility-test.c" \
                -o "$tmp_dir/test_cxx.o"
            checks=$((checks + 1))
        done
    done
done
echo "sys-process-param-visibility: PASS candidate PPU C++ (${#HEADERS[@]} headers x ${#CXX_MODES[@]} modes x 2 ABIs)"

# 3. Red control: unpatched cell/spurs.h without lv2_event_queue.h
red_spurs_dir="$tmp_dir/red_spurs/cell"
mkdir -p "$red_spurs_dir"
sed '/#include <cell\/spurs\/lv2_event_queue.h>/d' "$SDK_INC/cell/spurs.h" > "$red_spurs_dir/spurs.h"

set +e
red_spurs_out=$($PPU_CC -std=c99 -Wall -Wextra -Werror -Wno-pointer-to-int-cast \
    -I"$tmp_dir/red_spurs" "${inc_flags[@]}" \
    -DTEST_INCLUDE_CELL_SPURS=1 \
    -c "$SCRIPT_DIR/sys-process-param-visibility-test.c" \
    -o "$tmp_dir/red_spurs.o" 2>&1)
red_spurs_status=$?
set -e

if [ "$red_spurs_status" -eq 0 ]; then
    echo "sys-process-param-visibility: FAIL red control cell/spurs.h unexpectedly succeeded" >&2
    exit 1
fi
if ! printf '%s\n' "$red_spurs_out" | grep -E "(expected declaration specifiers|SYS_PROCESS_PARAM)" >/dev/null; then
    echo "sys-process-param-visibility: FAIL red control cell/spurs.h failed for wrong reason: $red_spurs_out" >&2
    exit 1
fi
echo "sys-process-param-visibility: PASS red control cell/spurs.h (missing process.h visibility caught)"

# 4. Red control: unpatched sys/dbg.h without sys/process.h
red_dbg_dir="$tmp_dir/red_dbg/sys"
mkdir -p "$red_dbg_dir"
sed '/#include <sys\/process.h>/d' "$SDK_INC/sys/dbg.h" > "$red_dbg_dir/dbg.h"

set +e
red_dbg_out=$($PPU_CC -std=c99 -Wall -Wextra -Werror -Wno-pointer-to-int-cast \
    -I"$tmp_dir/red_dbg" "${inc_flags[@]}" \
    -DTEST_INCLUDE_SYS_DBG=1 \
    -c "$SCRIPT_DIR/sys-process-param-visibility-test.c" \
    -o "$tmp_dir/red_dbg.o" 2>&1)
red_dbg_status=$?
set -e

if [ "$red_dbg_status" -eq 0 ]; then
    echo "sys-process-param-visibility: FAIL red control sys/dbg.h unexpectedly succeeded" >&2
    exit 1
fi
if ! printf '%s\n' "$red_dbg_out" | grep -E "(expected declaration specifiers|SYS_PROCESS_PARAM)" >/dev/null; then
    echo "sys-process-param-visibility: FAIL red control sys/dbg.h failed for wrong reason: $red_dbg_out" >&2
    exit 1
fi
echo "sys-process-param-visibility: PASS red control sys/dbg.h (missing process.h visibility caught)"

echo "sys-process-param-visibility: ALL PASS ($checks candidate configurations and 2 red controls passed)"
