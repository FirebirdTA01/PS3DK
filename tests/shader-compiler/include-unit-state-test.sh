#!/usr/bin/env bash
# t_d594ccd9: an #include'd header shares the unit's preprocessor state.
#
# Two defects, one cause.  The header was processed in a COPY of the
# preprocessor and the copy was discarded, so (1) every macro a header
# defined was invisible to the file that included it, and (2) a conditional
# left open at the header's end was refused as "Unterminated #if/#ifdef
# block" instead of continuing in the includer.  The reference keeps one
# conditional stack per translation unit: an include may end inside an open
# block, the includer's next #else/#endif binds to it, and a block still
# open at the end of the unit is closed silently; only a block still
# SKIPPING at the end of the unit warns (it swallowed the entry) and the
# program is then refused.  All four shapes were measured on the reference
# before this test was written.
#
# libretro's compat_macros.inc never closes its include guard and every
# shader in that corpus includes it: 472 of 804 survey refusals were this.
#
# Every accepting fixture is compared BYTE FOR BYTE with a control that has
# the macro written out, so the assertion is "the header's definition
# reached the includer", not merely "it compiled".
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
[[ -n "$compiler" ]] || compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-include-unit-state-test.$$"
mkdir -p "$work"; trap 'rm -rf "$work"' EXIT
shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"
limit="${PS3TC_INCLUDE_TEST_TIMEOUT:-20s}"

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

control=fp_include_macro_control_f
rc=0
run_one "$control" || rc=$?
[[ $rc -eq 0 && -s "$work/$control.fpo" ]] || { tail -n 5 "$work/$control.log" >&2; fail "control $control did not compile (exit $rc)"; }

must_match() {
    local stem="$1" why="$2"
    local rc=0
    run_one "$stem" || rc=$?
    [[ $rc -ne 124 ]] || fail "$stem TIMED OUT after $limit"
    [[ $rc -eq 0 ]] || { tail -n 5 "$work/$stem.log" >&2; fail "$stem exited $rc; the reference accepts it ($why)"; }
    [[ -s "$work/$stem.fpo" ]] || fail "$stem exited 0 but wrote no container"
    cmp -s "$work/$stem.fpo" "$work/$control.fpo" \
        || fail "$stem: container differs from $control - the header's macro did not reach the includer ($why)"
    printf '  %-36s == %s\n' "$stem" "$control"
}

must_match fp_include_closed_macro_f     "a macro defined in a well-formed header is visible to the includer"
must_match fp_include_open_conditional_f "a block left open by the header continues in the includer and its #endif closes it"
must_match fp_include_open_at_eof_f      "a block still open at the end of the unit is closed silently"

# NEGATIVE: the unclosed guard included twice leaves the unit skipping at its
# end; the entry was swallowed.  Refuse with exit 1, no container, and say
# so.  This is what keeps "one stack per unit" from meaning "anything goes".
stem=fp_include_open_twice_f
rc=0
run_one "$stem" || rc=$?
[[ $rc -ne 124 ]] || fail "$stem TIMED OUT after $limit"
[[ $rc -eq 1 ]] || fail "$stem exited $rc; a unit that ends while skipping must be an ordinary refusal (exit 1), as the reference does"
[[ ! -s "$work/$stem.fpo" ]] || fail "$stem refused but still wrote a container"
grep -q 'unmatched #if' "$work/$stem.log" \
    || { tail -n 5 "$work/$stem.log" >&2; fail "$stem refused without saying that an unmatched #if swallowed the rest of the unit"; }
printf '  %-36s refused: unmatched #if\n' "$stem"

printf 'include-unit-state-test: ok (header macros reach the includer; open conditionals continue across the include and close at unit end; skipping at unit end refuses)\n'
printf 'PASS: include-unit-state-test\n'
