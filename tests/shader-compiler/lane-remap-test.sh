#!/usr/bin/env bash
# lane-remap-test: builds tests/shader-compiler/lane-remap-test.cpp against
# src/lane_remap.h (dependency-free pure functions), runs it, and prints the
# case totals.  Exit 0 iff every case passed; exit 1 otherwise.
#
# Honors RSX_SYSROOT (path to a self-contained sysroot for a cross toolchain);
# if unset, falls back to the host default.  Honors CXX (defaults to c++).
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
work="$(mktemp -d "${TMPDIR:-/tmp}/ps3dk-lane-remap.XXXXXX")"
trap 'rm -rf "$work"' EXIT
src="$repo_root/tools/rsx-cg-compiler/src"
src_cpp="$repo_root/tests/shader-compiler/lane-remap-test.cpp"

if [[ -n "${RSX_SYSROOT:-}" ]]; then
    "${CXX:-c++}" -std=c++17 -O0 -w --sysroot="$RSX_SYSROOT" \
        -I"$src" "$src_cpp" -o "$work/lane-remap-test"
else
    "${CXX:-c++}" -std=c++17 -O0 -w \
        -I"$src" "$src_cpp" -o "$work/lane-remap-test"
fi

exec "$work/lane-remap-test"
