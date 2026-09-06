#!/usr/bin/env bash
# t_a290c3c8 Task 10: static fragment loops whose literal bounds include
# unary minus must unroll before NV40 lowering.
#
# The production break this catches: IRBuilder::tryUnrollStaticFor only
# accepts ExprKind::Literal integers.  Parser output for -1 is
# Unary(Negate, Literal(1)), so a reference-accepted 3x3 loop falls
# through to the general backend as a real back-edge and refuses.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-static-loop-unroll-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"
good_src="$shaders/generic_mad_chain_f.cg"
static_src="$shaders/fp_static_negative_loop_f.cg"
dynamic_src="$shaders/fp_refusal_dynamic_loop_f.cg"
short_circuit_src="$shaders/fp_short_circuit_sqrt_refuse_f.cg"
precomputed_sqrt_src="$shaders/fp_short_circuit_precomputed_sqrt_f.cg"

[[ -f "$static_src" ]] || fail "fixture missing: $static_src"
[[ -f "$short_circuit_src" ]] || fail "fixture missing: $short_circuit_src"
[[ -f "$precomputed_sqrt_src" ]] || fail "fixture missing: $precomputed_sqrt_src"

compile() {
    local label="$1" src="$2" out="$3" log="$4"
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx --dump-ir --emit-container "$out" "$src"
    ) >"$log" 2>&1 || {
        tail -n 30 "$log" >&2
        fail "$label did not compile"
    }
    [[ -s "$out" ]] || fail "$label produced no container"
}

compile success-control "$good_src" "$work/good.fpo" "$work/good.log"
compile static-negative-loop "$static_src" "$work/static.fpo" "$work/static.log"
compile precomputed-sqrt-predicate "$precomputed_sqrt_src" \
    "$work/precomputed.fpo" "$work/precomputed.log"

if grep -Eq '^for\.(cond|body|inc|end)[0-9]*:' "$work/static.log"; then
    awk '/define void @main/,/^}/' "$work/static.log" >&2
    fail "static-negative-loop left generated loop blocks in IR; expected full frontend unroll"
fi

out="$work/dynamic.fpo"
log="$work/dynamic.log"
rm -f "$out"
rc=0
(
    ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
    timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
        -p sce_fp_rsx --emit-container "$out" "$dynamic_src"
) >"$log" 2>&1 || rc=$?
[[ "$rc" -ne 0 ]] || fail "dynamic loop compiled; data-dependent fragment loops must still refuse"
[[ ! -e "$out" ]] || fail "dynamic loop left a container behind after refusal"
grep -q "back-edge" "$log" || {
    tail -n 20 "$log" >&2
    fail "dynamic loop refused for a reason other than the back-edge guard"
}

out="$work/short_circuit.fpo"
log="$work/short_circuit.log"
rm -f "$out"
rc=0
(
    ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
    timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
        -p sce_fp_rsx --emit-container "$out" "$short_circuit_src"
) >"$log" 2>&1 || rc=$?
[[ "$rc" -eq 0 && -s "$out" ]] || {
    tail -n 20 "$log" >&2
    fail "short-circuit sqrt comparison did not compile; reference uses an eager root then boolean comparison"
}

printf 'static-loop-unroll-test: ok\n'
