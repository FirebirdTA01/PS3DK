#!/usr/bin/env bash
# A matrix uniform's row indexes to a const register, on BOTH paths
# (t_9da20b33).  `m[0]` used to refuse everywhere - "StoreOutput source is
# not a direct Load or matvecmul" on the default path, "operand could not
# be resolved" on the general one - although the reference compiles it to
# one instruction, `MOV o[8], c[256]`.
#
# Two fixtures that differ only in the row, because the interesting way to
# get this wrong is not to refuse.  The row arrives as OPERAND 1 of the
# extract and both rows carry componentIndex 0, so a lowering that read
# the field would compile m[0] and m[2] to IDENTICAL ucode - the wrong row,
# silently.  Comparing the two shaders catches that where compiling them
# does not.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-vp-matrix-row-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"
for f in vp_matrix_row0_v.cg vp_matrix_row2_v.cg; do
    [[ -f "$shaders/$f" ]] || fail "fixture missing: $shaders/$f"
done

# $1 fixture stem, $2 "" | --general-lowering, $3 tag
compile() {
    local rc=0
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_vp_rsx ${2:+$2} "$shaders/$1.cg"
    ) >"$work/$3.log" 2>&1 || rc=$?
    [[ "$rc" -eq 124 ]] && fail "$3 timed out"
    if [[ "$rc" -ne 0 ]]; then
        tail -n 20 "$work/$3.log" >&2
        fail "$3 did not compile.  A matrix row is a const register the
reference reads directly; refusing it is the defect (t_9da20b33)."
    fi
    # The ucode rows alone, so the comparison below is about instructions
    # and not about anything else the compiler prints.
    grep -E '^ +[0-9]+:' "$work/$3.log" > "$work/$3.ucode" || true
    [[ -s "$work/$3.ucode" ]] || fail "$3 emitted no ucode"
}

compile vp_matrix_row0_v ""                   row0_default
compile vp_matrix_row2_v ""                   row2_default
compile vp_matrix_row0_v --general-lowering   row0_general
compile vp_matrix_row2_v --general-lowering   row2_general

for path in default general; do
    if cmp -s "$work/row0_$path.ucode" "$work/row2_$path.ucode"; then
        cat "$work/row0_$path.ucode" >&2
        fail "on the $path path m_auto[0] and m_auto[2] compile to the SAME
ucode, so the row index never reached the register.  Both extracts carry
componentIndex 0; the row is operand 1 (t_9da20b33)."
    fi
done

printf 'vp-matrix-row-test: ok\n'
