#!/usr/bin/env bash
# t_652d6e42: output-pinned SelPred must not allocate its destination in
# the same R slot as the condition or then-value source it reads later.
#
# SelPred expands as:
#   MOV dst, else
#   MOVC CC.x, cond
#   MOV dst(NE.x), then
#
# That is not read-then-write internally.  If dst aliases src0 or src1,
# the first MOV clobbers a value the later expanded instructions still
# need, producing an always-else select.  The fixture is a tracked copy of
# the test_76 shape that exposed this after control-flow lowering started
# composing COLOR0 lane-by-lane into output-pinned R0.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

# Prove the trace parser on safe adjacent records and genuine collisions.
python3 "$repo_root/tests/shader-compiler/selpred-alias-parser-test.py"

work="${TMPDIR:-/tmp}/ps3dk-selpred-output-alias-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

src="$repo_root/tools/rsx-cg-compiler/tests/shaders/fp_selpred_output_alias_f.cg"
out="$work/fp_selpred_output_alias.fpo"
log="$work/fp_selpred_output_alias.log"
[[ -f "$src" ]] || fail "fixture missing: $src"

(
    ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
    timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" env RSX_DUMP_ORDER=1 \
        "$compiler" -p sce_fp_rsx --emit-container "$out" "$src"
) >"$log" 2>&1 || {
    tail -n 30 "$log" >&2
    fail "fp_selpred_output_alias_f.cg did not compile"
}

[[ -s "$out" ]] || fail "compiler exited 0 but produced no container"

python3 "$repo_root/tests/shader-compiler/selpred_alias_check.py" "$log"

printf 'PASS: selpred-output-alias-test\n'
