#!/usr/bin/env bash
# t_3603033d: inject final allocations into the real emitter, including aliases.
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
work="$(mktemp -d "${TMPDIR:-/tmp}/ps3dk-selpred-emission.XXXXXX")"
trap 'rm -rf "$work"' EXIT
src="$repo_root/tools/rsx-cg-compiler/src"
"${CXX:-c++}" -std=c++17 -O0 -w -ffunction-sections -fdata-sections \
    -I"$src" -I"$src/nv40" -I"$src/donor" -I"$src/donor/ir" \
    -I"$src/donor/frontend" -I"$src/donor/common" \
    "$repo_root/tests/shader-compiler/selpred-emission-guard-test.cpp" \
    "$src/nv40/nv40_fp_assembler.cpp" \
    -Wl,--gc-sections -o "$work/selpred-emission-guard-test"
"$work/selpred-emission-guard-test"
