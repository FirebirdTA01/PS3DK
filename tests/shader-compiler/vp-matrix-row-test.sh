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

# $1 fixture stem, $2 flags, $3 tag
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

# Shelf-life: when the retired legacy matcher is removed, drop this second
# --legacy-lowering run and its header claim in the same commit.
compile vp_matrix_row0_v ""                   row0_general
compile vp_matrix_row2_v ""                   row2_general
compile vp_matrix_row0_v --legacy-lowering    row0_legacy
compile vp_matrix_row2_v --legacy-lowering    row2_legacy

for path in general legacy; do
    if cmp -s "$work/row0_$path.ucode" "$work/row2_$path.ucode"; then
        cat "$work/row0_$path.ucode" >&2
        fail "on the $path path m_auto[0] and m_auto[2] compile to the SAME
ucode, so the row index never reached the register.  Both extracts carry
componentIndex 0; the row is operand 1 (t_9da20b33)."
    fi
done

# A row of a float3x3, widened by a constructor: three lanes from the const
# register and one literal.  A constructor that counted operands instead of
# components would write o[8].x and o[8].y and lose the row's y and z.
compile vp_matrix_row_small_v ""                 small_general
compile vp_matrix_row_small_v --legacy-lowering  small_legacy

python3 - "$work/small_general.ucode" "$work/small_legacy.ucode" <<'PY'
import re
import sys


def masks(path):
    out = []
    for line in open(path, "r", encoding="utf-8"):
        m = re.match(r"\s*(\d+):((?:\s+[0-9a-fA-F]{8})+)\s*$", line)
        if not m:
            continue
        words = [int(w, 16) for w in m.group(2).split()]
        out.append((words[3] >> 13) & 0xF)
    return sorted(out)


# Four DP4 lanes into the position (0x1, 0x2, 0x4, 0x8) plus the texcoord's
# xyz (0xe) and w (0x1).
want = [0x1, 0x1, 0x2, 0x4, 0x8, 0xE]
for path, name in zip(sys.argv[1:3], ("general", "legacy")):
    got = masks(path)
    if got != want:
        raise SystemExit(
            "FAIL: on the %s path float4(m3[2], 1.0f) must write the row's "
            "three lanes together (0xe) and the literal into w (0x1); write "
            "masks were [%s], expected [%s].  A row is a const source THREE "
            "lanes wide (t_9da20b33)."
            % (name, ", ".join("0x%x" % m for m in got),
               ", ".join("0x%x" % m for m in want))
        )
PY

# Row 3 of a float3x3 is out of bounds - the reference calls it that and
# refuses.  Compiling it would read a register the matrix does not own.
for flags_tag in ":oob_general" "--legacy-lowering:oob_legacy"; do
    flags="${flags_tag%%:*}"
    tag="${flags_tag##*:}"
    rc=0
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_vp_rsx ${flags:+$flags} \
            "$shaders/vp_matrix_row_oob_v.cg"
    ) >"$work/$tag.log" 2>&1 || rc=$?
    [[ "$rc" -eq 124 ]] && fail "$tag timed out"
    if [[ "$rc" -eq 0 ]]; then
        tail -n 10 "$work/$tag.log" >&2
        fail "$tag COMPILED m3[3] on a float3x3.  The matrix owns three
registers; row 3 is the next allocation's, and the reference rejects the
source as an out-of-bounds index.  The row index must be bounded by the
matrix's own row count (t_9da20b33)."
    fi
done

printf 'vp-matrix-row-test: ok\n'
