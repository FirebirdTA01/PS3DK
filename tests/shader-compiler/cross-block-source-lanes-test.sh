#!/usr/bin/env bash
# t_652d6e42: source-only lane extracts reused across flattened blocks
# must resolve as sources, not as block-local temporaries.
#
# The fixture mirrors the operand-resolution shape in test_48_refract:
# input.uv.x/y are first materialized in one arm of a forward-only branch,
# then reused by the sibling arm after flattening.  These are pure source
# swizzles of TEXCOORD0, so resolving them lazily is safe and avoids a
# false "operand could not be resolved" refusal.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-cross-block-source-lanes-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

src="$repo_root/tools/rsx-cg-compiler/tests/shaders/fp_cross_block_source_lanes_f.cg"
out="$work/fp_cross_block_source_lanes.fpo"
log="$work/fp_cross_block_source_lanes.log"
[[ -f "$src" ]] || fail "fixture missing: $src"

(
    ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
    timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
        -p sce_fp_rsx --emit-container "$out" "$src"
) >"$log" 2>&1 || {
    tail -n 30 "$log" >&2
    fail "fp_cross_block_source_lanes_f.cg did not compile"
}

[[ -s "$out" ]] || fail "compiler exited 0 but produced no container"

printf 'PASS: cross-block-source-lanes-test\n'
