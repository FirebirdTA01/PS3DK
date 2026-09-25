#!/usr/bin/env bash
# spu-header-visibility-test.sh — Test matrix for SPU header visibility & C++ compatibility
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

SPU_CXX="${SPU_CXX:-spu-elf-g++}"

if ! command -v "$SPU_CXX" >/dev/null 2>&1; then
    if [ -n "${PS3DEV:-}" ] && [ -x "$PS3DEV/spu/bin/spu-elf-g++" ]; then
        SPU_CXX="$PS3DEV/spu/bin/spu-elf-g++"
    else
        echo "spu-header-visibility: SKIP (SPU cross-compiler unavailable)"
        exit 0
    fi
fi

# Include paths for SPU job headers and spu_event
SPU_JOB_INC="$ROOT_DIR/sdk/libspurs_job/include"
SPU_THREAD_INC="$ROOT_DIR/sdk/libsputhread/include"
SDK_INC="$ROOT_DIR/sdk/include"

CXX_MODES=("-std=gnu++98" "-std=c++98" "-std=c++11" "-std=c++14" "-std=c++17")

tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT

checks=0

echo "=== spu-header-visibility-test: starting ==="

# 1. Candidate GREEN: PROBE_JOB_CHAIN across C++ standards
for cxx_mode in "${CXX_MODES[@]}"; do
    $SPU_CXX $cxx_mode -Wall -Wextra -Werror -fsyntax-only \
        -DPROBE_JOB_CHAIN \
        -I"$SPU_JOB_INC" \
        -isystem "$SDK_INC" \
        "$SCRIPT_DIR/spu-header-visibility-test.cpp"
    checks=$((checks + 1))
done
echo "spu-header-visibility: PASS candidate PROBE_JOB_CHAIN (${#CXX_MODES[@]} standards)"

# 2. Candidate GREEN: PROBE_SPU_EVENT across C++ standards
for cxx_mode in "${CXX_MODES[@]}"; do
    $SPU_CXX $cxx_mode -Wall -Wextra -Werror -fsyntax-only \
        -DPROBE_SPU_EVENT \
        -I"$SPU_THREAD_INC" \
        -isystem "$SDK_INC" \
        "$SCRIPT_DIR/spu-header-visibility-test.cpp"
    checks=$((checks + 1))
done
echo "spu-header-visibility: PASS candidate PROBE_SPU_EVENT (${#CXX_MODES[@]} standards)"

# 3. Candidate GREEN: PROBE_VMX2SPU across C++ standards
for cxx_mode in "${CXX_MODES[@]}"; do
    $SPU_CXX $cxx_mode -Wall -Wextra -Werror -fsyntax-only \
        -DPROBE_VMX2SPU \
        -isystem "$SDK_INC" \
        "$SCRIPT_DIR/spu-header-visibility-test.cpp"
    checks=$((checks + 1))
done
echo "spu-header-visibility: PASS candidate PROBE_VMX2SPU (${#CXX_MODES[@]} standards)"

# 4. Red control: PROBE_JOB_CHAIN on unpatched job_chain.h (without stddef.h)
red_job_dir="$tmp_dir/red_job/cell/spurs"
mkdir -p "$red_job_dir"
sed '/#include <stddef.h>/d' "$SPU_JOB_INC/cell/spurs/job_chain.h" > "$red_job_dir/job_chain.h"

set +e
red_job_out=$($SPU_CXX -std=c++11 -Wall -Wextra -Werror -fsyntax-only \
    -DPROBE_JOB_CHAIN \
    -I"$tmp_dir/red_job" \
    -I"$SPU_JOB_INC" \
    -isystem "$SDK_INC" \
    "$SCRIPT_DIR/spu-header-visibility-test.cpp" 2>&1)
red_job_status=$?
set -e

if [ "$red_job_status" -eq 0 ]; then
    echo "spu-header-visibility: FAIL red control job_chain.h unexpectedly succeeded" >&2
    exit 1
fi
if ! printf '%s\n' "$red_job_out" | grep -E "(declaration of 'operator new' as non-function|'size_t' has not been declared)" >/dev/null; then
    echo "spu-header-visibility: FAIL red control job_chain.h failed for wrong reason: $red_job_out" >&2
    exit 1
fi
echo "spu-header-visibility: PASS red control job_chain.h (missing stddef.h caught)"

# 5. Red control: PROBE_SPU_EVENT on unpatched sys/spu_event.h (without stddef.h / sys/types.h)
red_event_dir="$tmp_dir/red_event/sys"
mkdir -p "$red_event_dir"
sed -e '/#include <stddef.h>/d' -e '/#include <sys\/types.h>/d' "$SPU_THREAD_INC/sys/spu_event.h" > "$red_event_dir/spu_event.h"

set +e
red_event_out=$($SPU_CXX -std=c++11 -Wall -Wextra -Werror -fsyntax-only \
    -DPROBE_SPU_EVENT \
    -I"$tmp_dir/red_event" \
    -I"$SPU_THREAD_INC" \
    -isystem "$SDK_INC" \
    "$SCRIPT_DIR/spu-header-visibility-test.cpp" 2>&1)
red_event_status=$?
set -e

if [ "$red_event_status" -eq 0 ]; then
    echo "spu-header-visibility: FAIL red control spu_event.h unexpectedly succeeded" >&2
    exit 1
fi
if ! printf '%s\n' "$red_event_out" | grep -E "(declaration of 'operator new' as non-function|'size_t' has not been declared)" >/dev/null; then
    echo "spu-header-visibility: FAIL red control spu_event.h failed for wrong reason: $red_event_out" >&2
    exit 1
fi
echo "spu-header-visibility: PASS red control spu_event.h (missing stddef.h caught)"

# 6. Red control: PROBE_VMX2SPU on actual compiler vmx2spu.h with patch 0005 reverted
compiler_vmx=$($SPU_CXX -print-file-name=include/vmx2spu.h)
if [ ! -f "$compiler_vmx" ]; then
    echo "spu-header-visibility: FAIL could not locate compiler vmx2spu.h" >&2
    exit 1
fi
red_vmx_dir="$tmp_dir/red_vmx"
mkdir -p "$red_vmx_dir"
cp "$compiler_vmx" "$red_vmx_dir/vmx2spu.h"
# Revert patch 0005: unparenthesize compound literal in vec_sum2s
sed -i 's/((vec_int4){0, -1, 0, -1})/(vec_int4){0, -1, 0, -1}/' "$red_vmx_dir/vmx2spu.h"

set +e
red_vmx_out=$($SPU_CXX -std=c++11 -Wall -Wextra -Werror -Wno-narrowing -fsyntax-only \
    -DPROBE_VMX2SPU \
    -I"$red_vmx_dir" \
    -isystem "$SDK_INC" \
    "$SCRIPT_DIR/spu-header-visibility-test.cpp" 2>&1)
red_vmx_status=$?
set -e

if [ "$red_vmx_status" -eq 0 ]; then
    echo "spu-header-visibility: FAIL red control vmx2spu.h unexpectedly succeeded" >&2
    exit 1
fi
if ! printf '%s\n' "$red_vmx_out" | grep -E 'macro "spu_and" passed 5 arguments' >/dev/null; then
    echo "spu-header-visibility: FAIL red control vmx2spu.h failed for wrong reason: $red_vmx_out" >&2
    exit 1
fi
echo "spu-header-visibility: PASS red control vmx2spu.h (reverted patch caught macro arity error)"

echo "spu-header-visibility: ALL PASS ($checks candidate configurations and 3 red controls passed)"
