#!/usr/bin/env bash
# refusal-semantics-test.sh — a refused compile must refuse ALL the way.
#
# Pins the contract that a shader the compiler cannot lower honestly
# produces (a) a nonzero exit code, (b) NO container file, and (c) a
# printed refusal line — on BOTH lowering paths.  The shipped v0.12.21
# --general-lowering violated (a) and (b): it printed "unsupported IR
# op frac" twice, then wrote a 416-byte container and exited 0.  A
# diagnostic nobody's exit code honours is not a refusal, and a
# container written past one is a plausible artifact where there
# should have been nothing.
#
# The fixture's refusal is PERMANENT by construction (data-dependent
# loop bound: NV40 FP has no flow control and the bound defeats
# unrolling), so this test does not expire as lowering coverage grows.
#
# The success control compiles a known-good shader through the same
# harness first: a test that cannot see success cannot be trusted to
# see refusal (a broken compiler build would otherwise pass every
# refusal assertion trivially).

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"

fail() {
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

good_src="$repo_root/tools/rsx-cg-compiler/tests/shaders/generic_mad_chain_f.cg"
bad_src="$repo_root/tools/rsx-cg-compiler/tests/shaders/fp_refusal_dynamic_loop_f.cg"
work="${TMPDIR:-/tmp}/ps3dk-refusal-semantics-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

run_compile() {
    # run_compile <label> <out-file> <log-file> <extra flags...>
    local label="$1" out="$2" log="$3" src="$4"
    shift 4
    local rc=0
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            "$@" -p sce_fp_rsx --emit-container "$out" "$src"
    ) >"$log" 2>&1 || rc=$?
    if [[ "$rc" -eq 124 ]]; then
        fail "$label timed out"
    fi
    if [[ "$rc" -eq 134 || "$rc" -eq 137 ]]; then
        fail "$label aborted or was killed under memory cap"
    fi
    return "$rc"
}

for path_flag in "--general-lowering" ""; do
    label="path[${path_flag:-default}]"
    # shellcheck disable=SC2086 — an empty flag must expand to nothing
    extra=($path_flag)

    # Success control first: the harness must be able to SEE a good
    # compile or its refusal verdicts mean nothing.
    out="$work/good.fpo"; log="$work/good.log"
    rm -f "$out"
    rc=0
    run_compile "$label success-control" "$out" "$log" "$good_src" ${extra[@]+"${extra[@]}"} || rc=$?
    [[ "$rc" -eq 0 ]] || { tail -n 10 "$log" >&2; fail "$label success-control did not compile (rc=$rc)"; }
    [[ -s "$out" ]] || fail "$label success-control produced no container"

    # The refusal, asserted in full.
    out="$work/bad.fpo"; log="$work/bad.log"
    rm -f "$out"
    rc=0
    run_compile "$label refusal" "$out" "$log" "$bad_src" ${extra[@]+"${extra[@]}"} || rc=$?
    [[ "$rc" -ne 0 ]] || fail "$label refusal exited 0 — a refused compile must fail the exit code"
    [[ ! -e "$out" ]] || fail "$label refusal left a container behind — a refusal that emits is not a refusal"
    grep -Eqi 'unsupported|refus|emit failed' "$log" \
        || { tail -n 10 "$log" >&2; fail "$label refusal printed no recognizable refusal line"; }
done

printf 'refusal-semantics-test: ok\n'
