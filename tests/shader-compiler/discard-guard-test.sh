#!/usr/bin/env bash
# CF-2 (t_91bbd575): `discard` on the general path.
#
# A discard's guard is the PATH CONDITION that reaches it, not the last
# comparison the emitter happened to walk past.  materialiseDiscardGuards
# computes it while the control flow is still intact and attaches it to
# the instruction; the lowering then emits the reference's shape - the
# guard's producing instruction retargeted to the condition register with
# cc_update, then KIL testing that register.
#
# Every expectation here was measured against the reference before the
# fixture that carries it was written.  The two that look like mistakes
# are not: a lone `>=` stays fp32 where the other five comparisons demote
# to fx12, and a negated guard flips the KIL's test instead of inverting
# the comparison.  Both are pinned so a later tidy-up cannot normalise
# them away with every other test still green.
#
# Assertions are on the DECODED UCODE.  A container's parameter table and
# input mask are not evidence about what the program does - t_e89cd261 was
# a mask naming a varying no instruction read.
#
# Three fixtures assert a REFUSAL, and each refusal is checked for its own
# reason so it cannot pass because something else broke first:
#
#   fp_discard_loop_f      - a back-edge, on the general path.  A
#                            DIVERGENCE, not a shared limit: the reference
#                            compiles a dynamic loop with real hardware
#                            loop instructions.  The bound must be
#                            dynamic; our frontend fully unrolls a
#                            constant one, so the refusal would never be
#                            reached.
#   fp_discard_else_f      - on the LEGACY path (the matcher), t_79fc6bf7.  EXPIRES
#                            when the matcher is retired; the general path
#                            compiles this shape correctly and is checked
#                            for it above (case else_arm).
#   fp_store_skippable_f   - on the general path: a store the control flow
#                            can SKIP with nothing killing the path that
#                            misses it.  In a flattened program every
#                            block runs, so such a store commits a value
#                            the branch was there to suppress.  It has no
#                            discard in it at all, which is what makes it
#                            the BOUND on that rule rather than another
#                            example of it.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-discard-guard.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"
here="$repo_root/tests/shader-compiler"

run() {   # $1 stem, $2 flags, $3 tag -> rc in $rc, output in $work/$3.log
    rc=0
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx ${2:+$2} "$shaders/$1.cg"
    ) >"$work/$3.log" 2>&1 || rc=$?
    if [[ "$rc" -eq 124 ]]; then fail "$3 timed out"; fi
    return 0
}

shape() {   # $1 fixture stem, $2 case name
    [[ -f "$shaders/$1.cg" ]] || fail "fixture missing: $shaders/$1.cg"
    run "$1" "" "$2"
    if [[ "$rc" -ne 0 ]]; then
        tail -n 20 "$work/$2.log" >&2
        fail "$1 did not compile on the general path.  CF-2 lowers the
discard shapes; a refusal here is the item reopening."
    fi
    ( cd "$here" && python3 discard_shapes.py "$2" "$work/$2.log" )
}

shape fp_discard_lt_f        lt
shape fp_discard_ge_f        ge
shape fp_discard_ge_rev_f    ge_rev
shape fp_discard_not_f       not
shape fp_discard_uncond_f    uncond
shape fp_discard_and_f       and
shape fp_discard_or_f        or
shape fp_discard_nested_f    nested
shape fp_discard_merge_f     merge
shape fp_discard_ops_f       ops
shape fp_discard_two_f       two
shape fp_discard_else_f      else_arm
shape fp_discard_then_work_f then_work

# One $kill_NNNN container parameter per discard STATEMENT.  Counted on
# the container's bytes, because that is where the runtime reads them.
kills_in_container() {   # $1 stem, $2 flags -> count on stdout
    local out="$work/$1.container"
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx ${2:+$2} --emit-container "$out" "$shaders/$1.cg"
    ) >/dev/null 2>&1 || fail "$1 did not emit a container"
    LC_ALL=C grep -a -o '[$]kill_' "$out" | wc -l | tr -d ' '
}

n="$(kills_in_container fp_discard_lt_f)"
[[ "$n" == "1" ]] || fail "fp_discard_lt_f container has $n \$kill parameters, expected 1"
n="$(kills_in_container fp_discard_ops_f)"
[[ "$n" == "6" ]] || fail "fp_discard_ops_f container has $n \$kill parameters, expected 6 - one per discard STATEMENT"
n="$(kills_in_container fp_discard_two_f)"
[[ "$n" == "2" ]] || fail "fp_discard_two_f container has $n \$kill parameters, expected 2"

# --- refusals, each checked for its own reason -------------------------

run fp_discard_loop_f "" loop
[[ "$rc" -ne 0 ]] || fail "fp_discard_loop_f compiled on the general path.
A discard inside a dynamic loop needs the back-edge CF-1a refuses.  If the
loop is being unrolled instead, the fixture's bound stopped being dynamic
and the refusal is no longer exercised."
grep -q "back-edge" "$work/loop.log" || {
    tail -n 5 "$work/loop.log" >&2
    fail "fp_discard_loop_f refused for some OTHER reason than the
back-edge; a refusal that fires for the wrong reason is not a guard."
}

run fp_discard_else_f "--legacy-lowering" else_legacy
[[ "$rc" -ne 0 ]] || fail "fp_discard_else_f compiled on the LEGACY path (the matcher).
That path recovers a discard's guard from the last comparison it walked
past, so on the false arm of a branch it kills exactly the fragments that
must survive (t_79fc6bf7).  It must refuse until the matcher is retired."
grep -q "FALSE arm" "$work/else_legacy.log" || {
    tail -n 5 "$work/else_legacy.log" >&2
    fail "fp_discard_else_f refused on the legacy path for some OTHER
reason than the false-arm discard."
}

run fp_discard_and_f "--legacy-lowering" and_legacy
[[ "$rc" -ne 0 ]] || fail "fp_discard_and_f compiled on the LEGACY path (the matcher).
Its guard compares a varying against a uniform: the pre-pass puts the
uniform in R1 and the varying's preload writes H2 with a full mask, and
H2's four fp16 lanes cover all of R1.x and R1.y - so the uniform is gone
before the second comparison reads it (t_ec804d32)."
grep -q "half-register preload" "$work/and_legacy.log" || {
    tail -n 5 "$work/and_legacy.log" >&2
    fail "fp_discard_and_f refused on the legacy path for some OTHER
reason than the half-preload aliasing."
}

run fp_discard_two_f "--legacy-lowering" two_legacy
[[ "$rc" -ne 0 ]] || fail "fp_discard_two_f compiled on the LEGACY path (the matcher).
That path emits the FIRST store to an output and drops the rest, so every
surviving fragment is painted the first value (t_becbfa69).  The
completeness check cannot see it - both varyings are read by the kills -
so the re-store itself is what must refuse."
grep -q "is stored" "$work/two_legacy.log" || {
    tail -n 5 "$work/two_legacy.log" >&2
    fail "fp_discard_two_f refused on the legacy path for some OTHER
reason than the re-stored output."
}

run fp_discard_merge_guard_f "--legacy-lowering" merge_guard_legacy
[[ "$rc" -ne 0 ]] || fail "fp_discard_merge_guard_f compiled on the DEFAULT
path.  The discard sits in a two-predecessor merge INSIDE an outer guarded
block: the inner branch cancels at the merge and the outer one does not, so
the guard is the outer condition and the kill would use the inner one.  The
first form of this refusal walked up, saw two predecessors and treated that
as 'nothing more guards this' - it stopped at the merge and passed on the
shape it was written for (codex's counterexample to 5bee678)."
grep -qE "cannot prove what guards|enclosing condition" "$work/merge_guard_legacy.log" || {
    tail -n 5 "$work/merge_guard_legacy.log" >&2
    fail "fp_discard_merge_guard_f refused on the legacy path for some
OTHER reason than an unproven or unaccounted guard."
}

run fp_discard_nested_f "--legacy-lowering" nested_legacy
[[ "$rc" -ne 0 ]] || fail "fp_discard_nested_f compiled on the LEGACY path (the matcher).
That path's guard is a single comparison, so an ENCLOSING branch is not
accounted for: if_convert collapses the inner if and leaves the discard in
the outer arm, and the kill then fires wherever the INNER condition holds,
including on fragments the outer branch never reached (t_7ae60244).  The
reference emits both comparisons and a multiply."
grep -q "enclosing condition" "$work/nested_legacy.log" || {
    tail -n 5 "$work/nested_legacy.log" >&2
    fail "fp_discard_nested_f refused on the legacy path for some OTHER
reason than the unaccounted enclosing branch."
}

# The two shapes the matcher gets RIGHT must keep compiling: a plain
# guard, and an `&&`.  if_convert hoists both and deletes the branch, so
# their whole condition IS the comparison the kill uses - and the
# discard-blend sample ships the `&&` shape byte-identical to the
# reference, so a rule that refused it would be a regression on a shipped
# sample rather than a guard.
for stem in fp_discard_lt_f fp_discard_ge_f fp_discard_then_work_f; do
    run "$stem" "--legacy-lowering" "${stem}_legacy_ok"
    [[ "$rc" -eq 0 ]] || {
        tail -n 5 "$work/${stem}_legacy_ok.log" >&2
        fail "$stem no longer compiles on the LEGACY path (the matcher).  The
enclosing-branch refusal is meant to catch a guard the path cannot
express, not the two it expresses correctly."
    }
done

run fp_store_skippable_f "" skippable
if [[ "$rc" -eq 0 ]]; then
    printf 'NOTE: fp_store_skippable_f now compiles on the general path.\n' >&2
    printf 'It has no discard in it, so nothing kills the path that misses\n' >&2
    printf 'its store: a value the branch was there to suppress is being\n' >&2
    printf 'committed unconditionally.  If the rule was relaxed on purpose,\n' >&2
    printf 'say what makes it safe and give it a shape assertion here.\n' >&2
    fail "fp_store_skippable_f: this test still asserts the refusal"
fi
grep -q "the control flow can skip" "$work/skippable.log" || {
    tail -n 5 "$work/skippable.log" >&2
    fail "fp_store_skippable_f refused on the general path for some OTHER
reason than the skippable store; the refusal moved and this test did not."
}

printf 'PASS: discard-guard-test\n'
