#!/usr/bin/env bash
# t_fe384b97: `inline` is a function qualifier the hardware compiler accepts
# and ignores.  The general path lexed it as an identifier and rejected it as
# an unknown type name, so every shader that declared an inline helper - 110
# of the internet survey's remaining refusals - failed to compile.  The
# reference emits the same program with or without inline, and refuses it on
# a non-function (C1005 "inline modifier only for functions").
#
# The accepting fixtures are compared BYTE FOR BYTE with an inline-free
# control, so the assertion is "inline changed nothing", not merely "it
# compiled".  The negative must refuse with exit 1 and no container.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
[[ -n "$compiler" ]] || compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-inline-qualifier-test.$$"
mkdir -p "$work"; trap 'rm -rf "$work"' EXIT
shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"

run_one() {
    local stem="$1"
    set +e
    ( ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
      timeout "${PS3TC_SHADER_TEST_TIMEOUT:-20s}" "$compiler" -p sce_fp_rsx \
          --emit-container "$work/$stem.fpo" "$shaders/$stem.cg" ) \
        >"$work/$stem.log" 2>&1
    local rc=$?
    set -e
    return $rc
}

# Compile the inline-free control once; every accepting fixture matches it.
control=fp_inline_function_control_f
rc=0
run_one "$control" || rc=$?
[[ $rc -eq 0 && -s "$work/$control.fpo" ]] || { tail -n 5 "$work/$control.log" >&2; fail "control $control did not compile (exit $rc)"; }

must_match() {
    local stem="$1" why="$2"
    local rc=0
    run_one "$stem" || rc=$?
    [[ $rc -eq 0 ]] || { tail -n 5 "$work/$stem.log" >&2; fail "$stem exited $rc; the reference accepts it ($why)"; }
    [[ -s "$work/$stem.fpo" ]] || fail "$stem exited 0 but wrote no container"
    cmp -s "$work/$stem.fpo" "$work/$control.fpo" \
        || fail "$stem: container differs from the inline-free control - inline was not a no-op ($why)"
    printf '  %-32s == %s\n' "$stem" "$control"
}

must_match fp_inline_function_f        "inline on a function is ignored"
must_match fp_inline_static_function_f "static inline is accepted and inline ignored"

# NEGATIVE: inline on a variable must be refused, as the reference does.
stem=fp_inline_variable_f
rc=0
run_one "$stem" || rc=$?
[[ $rc -eq 1 ]] || { tail -n 5 "$work/$stem.log" >&2; fail "$stem exited $rc; inline on a variable must be an ordinary refusal (exit 1), as the reference refuses it (C1005)"; }
[[ ! -s "$work/$stem.fpo" ]] || fail "$stem refused but still wrote a container"
grep -qi 'inline modifier only for functions' "$work/$stem.log" \
    || { tail -n 5 "$work/$stem.log" >&2; fail "$stem refused without naming the inline-on-non-function rule"; }
printf '  %-32s refused: inline modifier only for functions\n' "$stem"

# The same refusal on a LOCAL variable inside a function body: the statement
# path must read the inline flag too, not only the file-scope declaration.
stem=fp_inline_local_variable_f
rc=0
run_one "$stem" || rc=$?
[[ $rc -eq 1 ]] || { tail -n 5 "$work/$stem.log" >&2; fail "$stem exited $rc; inline on a local variable must be refused (exit 1), as the reference refuses it (C1005)"; }
[[ ! -s "$work/$stem.fpo" ]] || fail "$stem refused but still wrote a container"
grep -qi 'inline modifier only for functions' "$work/$stem.log" \
    || { tail -n 5 "$work/$stem.log" >&2; fail "$stem refused without naming the inline-on-non-function rule"; }
printf '  %-32s refused: inline modifier only for functions\n' "$stem"

printf 'inline-qualifier-test: ok (inline on a function is a no-op matching the control; inline on a variable is refused)\n'
printf 'PASS: inline-qualifier-test\n'
