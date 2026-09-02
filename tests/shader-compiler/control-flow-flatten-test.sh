#!/usr/bin/env bash
# control-flow-flatten-test.sh — CF-1a: forward-only control flow
# flattens on the general path, and ONLY on the general path.
#
# Pins the tier-a battery of docs/design/shader-compiler-control-flow.md
# §5 (board task t_91bbd575):
#
#   diamond          general: compiles    default: keeps refusing
#   nested diamond   general: compiles    default: keeps refusing
#   guarded divide   general: REFUSES     default: keeps refusing
#   back-edge (loop) general: REFUSES     (full both-path contract is
#                    pinned by refusal-semantics-test.sh; reasserted
#                    here because the dynamic-loop fixture doubles as
#                    the tier-a back-edge refusal witness)
#
# The default-path assertions are the GATE, not a convenience: the
# flatten runs only under --general-lowering, and a flattened shader
# newly compiling on the default path would be a verdict change the
# byte fence rejects.  The guarded-divide refusal is CF-1a's
# contamination guard — an arithmetic-blend select lowering would let
# the untaken arm's inf/NaN poison the join (0*inf != 0), so until
# CF-1b's predicated write lands, arms that are not provably finite
# must refuse rather than blend.  When CF-1b lands, the guarded-divide
# expectation here flips from refusal to success WITH a pixel-judged
# readback row as its acceptance (§5 tier c) — that flip is this
# test's designed expiry, stated so it does not surprise.
#
# Refusals are asserted in full per the refusal-semantics contract:
# nonzero exit AND no container file AND a printed refusal line.
# The success control compiles a known-good shader through the same
# harness first: a test that cannot see success cannot be trusted to
# see refusal.

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

shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"
good_src="$shaders/generic_mad_chain_f.cg"
diamond_src="$shaders/fp_cf_diamond_f.cg"
nested_src="$shaders/fp_cf_nested_diamond_f.cg"
guarded_src="$shaders/fp_cf_guarded_divide_f.cg"
loop_src="$shaders/fp_refusal_dynamic_loop_f.cg"
work="${TMPDIR:-/tmp}/ps3dk-cf-flatten-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

run_compile() {
    # run_compile <out-file> <log-file> <src> <extra flags...>
    local out="$1" log="$2" src="$3"
    shift 3
    local rc=0
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            "$@" -p sce_fp_rsx --emit-container "$out" "$src"
    ) >"$log" 2>&1 || rc=$?
    if [[ "$rc" -eq 124 ]]; then
        fail "compile timed out: $src"
    fi
    if [[ "$rc" -eq 134 || "$rc" -eq 137 ]]; then
        fail "compile aborted or was killed under memory cap: $src"
    fi
    return "$rc"
}

expect_success() {
    # expect_success <label> <src> <flags...>
    local label="$1" src="$2"
    shift 2
    local out="$work/ok.fpo" log="$work/ok.log" rc=0
    rm -f "$out"
    run_compile "$out" "$log" "$src" "$@" || rc=$?
    [[ "$rc" -eq 0 ]] || { tail -n 10 "$log" >&2; fail "$label did not compile (rc=$rc)"; }
    [[ -s "$out" ]] || fail "$label produced no container"
}

expect_refusal() {
    # expect_refusal <label> <reason-regex> <src> <flags...>
    # The reason regex pins WHY the compile refused, not just THAT it
    # refused — without it every refusal fixture is interchangeable
    # and a flatten that regressed into refusing everything generically
    # would still pass (review finding on b10513e).  Pass '' to assert
    # only the generic refusal contract.
    local label="$1" reason="$2" src="$3"
    shift 3
    local out="$work/no.fpo" log="$work/no.log" rc=0
    rm -f "$out"
    run_compile "$out" "$log" "$src" "$@" || rc=$?
    [[ "$rc" -ne 0 ]] || fail "$label exited 0 — this shape must refuse"
    [[ ! -e "$out" ]] || fail "$label left a container behind — a refusal that emits is not a refusal"
    grep -Eqi 'unsupported|refus|emit failed' "$log" \
        || { tail -n 10 "$log" >&2; fail "$label printed no recognizable refusal line"; }
    if [[ -n "$reason" ]]; then
        grep -Eqi "$reason" "$log" \
            || { tail -n 10 "$log" >&2; fail "$label refused for the wrong reason (wanted /$reason/)"; }
    fi
}

# Success control on both paths: the harness must be able to SEE a
# good compile or every verdict below means nothing.
expect_success "success-control[legacy]" "$good_src" --legacy-lowering
expect_success "success-control[general]" "$good_src" --general-lowering

# The unlock: forward-only diamonds compile on the general path.
expect_success "diamond[general]"        "$diamond_src" --general-lowering
expect_success "nested-diamond[general]" "$nested_src"  --general-lowering

# The gate: the same shaders keep refusing on the matcher.
expect_refusal "diamond[legacy]"        '' "$diamond_src" --legacy-lowering
expect_refusal "nested-diamond[legacy]" '' "$nested_src" --legacy-lowering

# CF-1b: non-provably-finite join arms lower as a PREDICATED WRITE
# (MOV default, CC-set from cond, CC-gated commit) instead of the
# contaminating arithmetic blend — the guarded divide compiles on the
# general path.  Pixel-level acceptance is the §5 tier-c readback row;
# this asserts the compile-level contract.  The matcher keeps
# refusing: nothing about CF-1 touches it.
expect_success "guarded-divide[general]" "$guarded_src" --general-lowering
expect_refusal "guarded-divide[legacy]" '' "$guarded_src" --legacy-lowering

# The back-edge witness: a loop must refuse loudly, never flatten
# wrong — and for its OWN reason, so this fixture cannot be satisfied
# by a generic refusal.
expect_refusal "back-edge[general]" 'back-edge' "$loop_src" --general-lowering

printf 'control-flow-flatten-test: ok\n'
