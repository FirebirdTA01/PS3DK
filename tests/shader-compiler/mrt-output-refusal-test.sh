#!/usr/bin/env bash
# t_652d6e42 fallout: the general fragment path does not implement MRT
# colour outputs yet.  A shader with COLOR1+ must refuse with a named
# reason instead of compiling a container that writes only the primary
# colour output and paints garbage (test_69_mrt_gbuffer exposed this once
# cross-block source lane resolution stopped being the first failure).
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-mrt-output-refusal-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"
good_src="$shaders/generic_mad_chain_f.cg"
mrt_src="$shaders/fp_mrt_outputs_refuse_f.cg"
color1_only_src="$shaders/fp_mrt_color1_only_refuse_f.cg"

out="$work/good.fpo"
log="$work/good.log"
(
    ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
    timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
        -p sce_fp_rsx --emit-container "$out" "$good_src"
) >"$log" 2>&1 || {
    tail -n 20 "$log" >&2
    fail "success control did not compile"
}
[[ -s "$out" ]] || fail "success control produced no container"

expect_mrt_refusal() {
    local label="$1" src="$2"
    local out="$work/$label.fpo"
    local log="$work/$label.log"
    rm -f "$out"
    local rc=0
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx --emit-container "$out" "$src"
    ) >"$log" 2>&1 || rc=$?

    [[ "$rc" -ne 0 ]] || fail "$label exited 0; unsupported secondary colour outputs must refuse"
    [[ ! -e "$out" ]] || fail "$label left a container behind; a refusal that emits is not a refusal"
    grep -Eqi 'multiple fragment colour outputs are not lowered' "$log" || {
        tail -n 20 "$log" >&2
        fail "$label refused for an unnamed or wrong reason"
    }
}

expect_mrt_refusal "mrt" "$mrt_src"
expect_mrt_refusal "color1_only" "$color1_only_src"

printf 'PASS: mrt-output-refusal-test\n'
