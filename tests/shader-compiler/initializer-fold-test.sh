#!/usr/bin/env bash
# t_10dc2936: arithmetic in a file-scope initialiser is folded the way the
# reference folds it, or refused - never compiled as a guess.
#
# Measured on sce-cgc 475 (2026-09-14, .local/probe-init, byte twins against
# hand-folded literals): integer division truncates toward zero (7/2 is 3,
# -7/2 is -3), 7%3 is 1, a float operand makes it single-precision float
# arithmetic rounded per operation ((16777216+1+1)-16777216 is 0), the brace
# and constructor spellings are identical, comparisons/ternary/shifts fold, a
# vector times a scalar folds lane by lane, a static const identifier folds.
# A non-static file-scope const is a uniform on the reference, so an
# initialiser reading one is C1059 there: a refusal here, never a fold.
#
# Every accepting row is a BYTE twin against a control whose value was folded
# by hand, so the assertion is "the value the reference would compute", not
# "it compiled".  Refusal rows assert exit status 1 exactly and the diagnostic
# class, so a crash or a silent zero cannot pass as a refusal.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
[[ -n "$compiler" ]] || compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="$(mktemp -d "${TMPDIR:-/tmp}/ps3dk-initializer-fold-test.XXXXXX")"
trap 'rm -rf "$work"' EXIT
shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"

# Compile one fixture; the output is removed first so a stale artifact can
# never satisfy a later comparison.
run_one() {
    local stem="$1" rc=0
    rm -f "$work/$stem.fpo"
    set +e
    "$compiler" -p sce_fp_rsx --emit-container "$work/$stem.fpo" "$shaders/$stem.cg" >"$work/$stem.log" 2>&1
    rc=$?
    set -e
    return $rc
}

twin() {
    local cell="$1" why="$2" rc=0
    local stem="fp_initfold_${cell}_f" control="fp_initfold_${cell}_control_f"
    run_one "$stem" || rc=$?
    [[ $rc -eq 0 ]] || { tail -n 4 "$work/$stem.log" >&2; fail "$stem exited $rc; the reference accepts it ($why)"; }
    [[ -s "$work/$stem.fpo" ]] || fail "$stem exited 0 but wrote no container"
    rc=0
    run_one "$control" || rc=$?
    [[ $rc -eq 0 && -s "$work/$control.fpo" ]] || fail "control $control did not compile (exit $rc)"
    cmp -s "$work/$stem.fpo" "$work/$control.fpo" || fail "$stem differs from its hand-folded control: $why"
    printf '  PASS  %-28s == hand-folded control\n' "$cell"
}

# A refusal: exit status 1 EXACTLY (0 is a silent fold, 124 a hang, >=128 a
# crash), NO output file at all (an empty one is a leak, not a refusal), and
# the diagnostic class in the log.
refuse() {
    local stem="$1" pattern="$2" why="$3" rc=0
    run_one "$stem" || rc=$?
    [[ $rc -ne 0 ]] || fail "$stem compiled; it must refuse ($why)"
    [[ $rc -eq 1 ]] || fail "$stem exited $rc, not the refusal status 1 ($why)"
    [[ ! -e "$work/$stem.fpo" ]] || fail "$stem refused but left an output file (even an empty one is a leak)"
    grep -qE "$pattern" "$work/$stem.log" || { tail -n 4 "$work/$stem.log" >&2; fail "$stem refused for another reason than '$pattern' ($why)"; }
    printf '  PASS  %-28s refused: %s\n' "$stem" "$why"
}

twin brace      "brace list {1.0f/1280.0f, 1.0f/720.0f} folds to the two literals (the SpuRender shape)"
twin ctor       "constructor spelling folds identically"
twin scalar     "3.0f * 0.5f + 1.0f is 2.5"
twin uniform    "a uniform's arithmetic default folds"
twin intdiv     "7 / 2 is integer division, 3"
twin intdivneg  "-7 / 2 truncates toward zero, -3"
twin mixdiv     "7 / 2.0f is float division, 3.5"
twin mod        "7 % 3 is 1"
twin third      "1.0f / 3.0f is the single-precision 0.33333334"
twin precision  "each operation rounds to single precision: (2^24 + 1 + 1) - 2^24 is 0, not 2"
twin nested     "(1 + 2) * 3 - 4 / 8 is 8.5"
twin ternary    "1 < 2 ? 0.5f : 0.25f is 0.5"
twin shift      "(1 << 4) | 3 is 19"
twin staticid   "a static const identifier folds inside another initialiser"
twin vecscalar  "float2(1,2) * 0.5f folds lane by lane"
twin uniformint "uniform int N = 4 * 2 defaults to 8"
twin terncommon "(true ? 7 : 2.0f) / 2 is 3.5: the ternary takes the COMMON type before the division"
twin ternfalse  "(false ? 7 : 2.0f) / 2 is 1.0"
twin ternint    "(true ? 7 : 2) / 2 stays integer division, 3"
twin ternvec    "true ? float2(1,2) : 3.0f is float2(1,2): scalar branch broadcasts to the vector shape"
twin matrow     "M[1] of a static const float2x2 is ROW 1, float2(3,4), not lane 1"
twin matelem    "M[1][0] is 3"
twin matrowswz  "M[1].y is 4"
twin arrayelem  "A[1] of a static const float[3] is element 1, 6"
twin arrayvec   "A[1] of a static const float2[2] is float2(3,4)"

refuse fp_initfold_nonstatic_ident_refuse_f "cannot evaluate" "a non-static const is a uniform on the reference; reading it is C1059 there"
refuse fp_initfold_uniform_ident_gap_f      "cannot evaluate" "NAMED GAP: the reference evaluates a static const from a uniform at run time; we do not yet"
refuse fp_initfold_divzero_refuse_f         "cannot evaluate" "NAMED GAP: the reference folds 1.0f/0.0f to a value not yet pinned; we refuse rather than guess"
refuse fp_initfold_toomuch_refuse_f         "constructor requires 2 components, but 3 were provided|cannot evaluate" "a brace list with more data than the declared type holds is C1058 on the reference (ours refuses at the constructor arity check)"

# Self-check for the refusal rows: a compiler that exits 1 but leaves an EMPTY
# output file must be caught as a leak, not accepted as a refusal.
leaky="$work/leaky-compiler.sh"
printf '#!/usr/bin/env bash
out=""; while [[ $# -gt 0 ]]; do [[ "$1" == "--emit-container" ]] && out="$2"; shift; done; : > "$out"; echo "refused" >&2; exit 1
' > "$leaky"
chmod +x "$leaky"
leak_rc=0
( compiler="$leaky"; refuse fp_initfold_toomuch_refuse_f "refused" "leak control" ) >"$work/leak-control.log" 2>&1 || leak_rc=$?
[[ $leak_rc -eq 1 ]] || { cat "$work/leak-control.log" >&2; fail "self-check: the leak control exited $leak_rc, not the row's own failure status 1"; }
grep -q "refused but left an output file" "$work/leak-control.log"     || { cat "$work/leak-control.log" >&2; fail "self-check: the leak control failed for another reason than the left-an-output-file assertion"; }
printf '  PASS  self-check: an empty output file fails the refusal row on the left-an-output-file assertion
'

echo "initializer-fold: PASS"
