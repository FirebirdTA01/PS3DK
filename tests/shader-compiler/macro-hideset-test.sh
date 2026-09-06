#!/usr/bin/env bash
# t_53363b4b: macro expansion must TERMINATE on a macro that names itself,
# directly or through another macro, and must expand it the way the
# reference does - once.
#
# The old expander re-tokenised the whole line after every substitution and
# could not remember what it had already expanded, so
#   #define dot(x,y) saturate(dot(x,y))      (libretro's "NVIDIA fix")
# and
#   #define A B / #define B A
# both spun for ever.  The reference accepts both.  The fix is the standard
# hide-set algorithm: every token carries the names it must not expand.
#
# THE TIMEOUT IS THE RED.  On the unfixed compiler these inputs never return,
# so this test treats exit 124 as its own failure with its own message.  A
# plain "compiled" check would be worthless here - the parent never gets as
# far as an exit code.  Do not simplify these assertions.
#
# Every accepting fixture is compared BYTE FOR BYTE with a control that has
# the macros expanded by hand, so the assertion is "expanded the way the
# language says", not merely "terminated".
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
[[ -n "$compiler" ]] || compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-macro-hideset-test.$$"
mkdir -p "$work"; trap 'rm -rf "$work"' EXIT
shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"
limit="${PS3TC_MACRO_TEST_TIMEOUT:-20s}"

# Compile one fixture; returns the exit status, never hangs the harness.
run_one() {
    local stem="$1"
    set +e
    ( ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
      timeout "$limit" "$compiler" -p sce_fp_rsx \
          --emit-container "$work/$stem.fpo" "$shaders/$stem.cg" ) \
        >"$work/$stem.log" 2>&1
    local rc=$?
    set -e
    return $rc
}

# An accepting fixture: exit 0, container written, byte-identical to its
# hand-expanded control.
must_match() {
    local stem="$1" control="$2" why="$3"
    local rc=0
    run_one "$stem" || rc=$?
    [[ $rc -ne 124 ]] || fail "$stem TIMED OUT after $limit - macro expansion did not terminate ($why; t_53363b4b)"
    [[ $rc -lt 128 ]] || fail "$stem died on a signal (exit $rc)"
    [[ $rc -eq 0 ]] || { tail -n 5 "$work/$stem.log" >&2; fail "$stem exited $rc; the reference accepts it ($why)"; }
    [[ -s "$work/$stem.fpo" ]] || fail "$stem exited 0 but wrote no container"
    rc=0
    run_one "$control" || rc=$?
    [[ $rc -eq 0 && -s "$work/$control.fpo" ]] || { tail -n 5 "$work/$control.log" >&2; fail "control $control did not compile (exit $rc)"; }
    cmp -s "$work/$stem.fpo" "$work/$control.fpo" \
        || fail "$stem: container differs from its hand-expanded control $control - the macro was not expanded the way the language says ($why)"
    printf '  %-36s == %s\n' "$stem" "$control"
}

must_match fp_macro_self_reference_f fp_macro_self_reference_control_f \
    "a macro naming itself must expand exactly once"
must_match fp_macro_mutual_cycle_f  fp_macro_mutual_cycle_control_f \
    "A->B->A must stop at the painted name"
must_match fp_macro_shapes_f        fp_macro_shapes_control_f \
    "token pasting, nesting, an object-like alias and a bare function-like name must keep working"

printf 'macro-hideset-test: ok (self-reference and mutual cycle terminate and match their controls; pasting, nesting and aliasing unchanged)\n'
printf 'PASS: macro-hideset-test\n'
