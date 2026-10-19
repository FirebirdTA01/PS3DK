#!/usr/bin/env bash
# malformed-scalar-operand-lanes: malformed scalar operand lanes must refuse in the real emitter.
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
work="$(mktemp -d "${TMPDIR:-/tmp}/ps3dk-scalar-emission.XXXXXX")"
trap 'rm -rf "$work"' EXIT
src="$repo_root/tools/rsx-cg-compiler/src"
"${CXX:-c++}" -std=c++17 -O0 -w -ffunction-sections -fdata-sections \
    -I"$src" -I"$src/nv40" -I"$src/cg_frontend" -I"$src/cg_frontend/ir" \
    -I"$src/cg_frontend/frontend" -I"$src/cg_frontend/common" \
    "$repo_root/tests/shader-compiler/scalar-emission-guard-test.cpp" \
    "$src/nv40/nv40_fp_assembler.cpp" \
    -Wl,--gc-sections -o "$work/scalar-emission-guard-test"
"$work/scalar-emission-guard-test"
