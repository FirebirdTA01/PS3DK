#!/usr/bin/env bash
# t_1704e79e: the preprocessor must predefine exactly what the reference
# predefines.  Measured on sce-cgc 475 (2026-09-14): __CGC__ and __SCE_CGC__
# are defined, both 20000; __CG__, _CG_, _CGC_, CGC, __psp2__, __SCE__,
# __STDC__ and __STDC_VERSION__ are not.
#
# The donor compiler predefined the PSVita set (__psp2__, __SCE__, __STDC__,
# __STDC_VERSION__) and none of the reference's, so a header written as
#   #ifdef __CGC__   <Cg declarations>   #else   <C declarations>   #endif
# took the C branch and died on 'enum' - five SDK shaders (SpuRender).
#
# Every row is a BYTE twin against a hand-expanded control, so the assertion
# is "the macro has the reference's value" and "the absent names are absent",
# not merely "it compiled".  A name that becomes defined adds its own power of
# two in the absent fixture and breaks the twin.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
[[ -n "$compiler" ]] || compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

# Fresh scratch every run: a PID-derived path plus mkdir -p could hand a stale
# container from an earlier run to cmp, so an exit-0/no-output compiler would
# pass. mktemp -d cannot collide, and compile() removes its output before every
# launch as a second wall.
work="$(mktemp -d "${TMPDIR:-/tmp}/ps3dk-predefined-macro-test.XXXXXX")"
trap 'rm -rf "$work"' EXIT
shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"

compile() {
    local stem="$1" rc=0
    rm -f "$work/$stem.fpo"
    set +e
    "$compiler" -p sce_fp_rsx --emit-container "$work/$stem.fpo" "$shaders/$stem.cg" >"$work/$stem.log" 2>&1
    rc=$?
    set -e
    [[ $rc -eq 0 ]] || { tail -n 5 "$work/$stem.log" >&2; fail "$stem exited $rc; the reference accepts it"; }
    [[ -s "$work/$stem.fpo" ]] || fail "$stem exited 0 but wrote no container"
}

twin() {
    local stem="$1" control="$2" why="$3"
    compile "$stem"; compile "$control"
    cmp -s "$work/$stem.fpo" "$work/$control.fpo" \
        || fail "$stem differs from its hand-expanded control $control: $why"
    printf '  PASS  %-28s == %s\n' "$stem" "$control"
}

twin fp_predef_values_f  fp_predef_values_control_f  "__CGC__ and __SCE_CGC__ must both be 20000"
twin fp_predef_absent_f  fp_predef_absent_control_f  "a name the reference does not define is defined here (each adds its own power of two)"
twin fp_predef_branch_f  fp_predef_branch_control_f  "#ifdef __CGC__ must take the Cg branch"

# The absent fixture is only a witness if a defined name really changes bytes:
# a twin that #defines one absent name itself must break the twin.
compile fp_predef_absent_defined_f
cmp -s "$work/fp_predef_absent_defined_f.fpo" "$work/fp_predef_absent_control_f.fpo"     && fail "self-check: defining __psp2__ in the source did not change the absent fixture's bytes, so the absent row cannot see a defined name"
printf '  PASS  self-check: a defined name is visible in the absent row
'
echo "predefined-macro: PASS"
