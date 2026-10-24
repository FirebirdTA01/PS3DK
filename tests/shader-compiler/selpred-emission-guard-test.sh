#!/usr/bin/env bash
# select-predication-record-boundary: inject final allocations into the real emitter, including aliases.
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
work="$(mktemp -d "${TMPDIR:-/tmp}/ps3dk-selpred-emission.XXXXXX")"
trap 'rm -rf "$work"' EXIT
src="$repo_root/tools/rsx-cg-compiler/src"
"${CXX:-c++}" -std=c++17 -O0 -w -ffunction-sections -fdata-sections \
    -I"$src" -I"$src/nv40" -I"$src/cg_frontend" -I"$src/cg_frontend/ir" \
    -I"$src/cg_frontend/frontend" -I"$src/cg_frontend/common" \
    "$repo_root/tests/shader-compiler/selpred-emission-guard-test.cpp" \
    "$src/nv40/nv40_fp_assembler.cpp" \
    -Wl,--gc-sections -o "$work/selpred-emission-guard-test"
"$work/selpred-emission-guard-test"
