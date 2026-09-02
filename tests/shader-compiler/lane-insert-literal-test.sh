#!/usr/bin/env bash
# A literal written into one lane of a varying passthrough must reach the
# ucode (t_afb4af65).  Before the fix the default path emitted a single
# full-width MOV of the varying and the insert was gone, with no
# diagnostic and a well-formed container.
#
# The assertions are on the EMITTED UCODE rather than on the exit status,
# because the defect's whole character is that the exit status was 0 and
# the container looked right.  What the shapes below check is what the
# reference compiler does, read off its own output:
#
#   one lane overridden : base MOV masked to the lanes the insert does NOT
#                         write, then a MOV of the literal into that lane
#   all lanes overridden: no read of the varying at all - and the
#                         completeness guard must not mistake a value the
#                         source overwrote for work that was dropped
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-lane-insert-literal-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

compile() {   # $1 shader path, $2 tag, $3 extra flags -> log at $work/$2.log
    local rc=0
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx ${3:+$3} "$1"
    ) >"$work/$2.log" 2>&1 || rc=$?
    [[ "$rc" -eq 124 ]] && fail "$2 timed out"
    [[ "$rc" -eq 134 || "$rc" -eq 137 ]] && fail "$2 aborted or was killed under the memory cap"
    if [[ "$rc" -ne 0 ]]; then
        tail -n 20 "$work/$2.log" >&2
        fail "$2 failed to compile"
    fi
}

one="$repo_root/tools/rsx-cg-compiler/tests/shaders/fp_insert_literal_f.cg"
all="$repo_root/tools/rsx-cg-compiler/tests/shaders/fp_insert_literal_all_f.cg"
[[ -f "$one" ]] || fail "fixture missing: $one"
[[ -f "$all" ]] || fail "fixture missing: $all"

compile "$one" one --legacy-lowering
compile "$all" all --legacy-lowering

# The GENERAL path refused this shape entirely - the insert's result was
# never defined and the store reported "operand could not be resolved"
# (t_be578e74).  It now materialises the varying into a temp masked to the
# lanes the insert does not write, which is the reference's own shape, so
# the one-lane case is judged by the same assertions as the matcher.
compile "$one" one_general --general-lowering
compile "$all" all_general --general-lowering
cmp -s "$work/one.log" "$work/one_general.log" || {
    diff "$work/one.log" "$work/one_general.log" >&2 || true
    fail "the general path's ucode for a single lane insert differs from the
default path's, and both are byte-identical to the reference there
(t_be578e74)."
}

python3 - "$work/one.log" "$work/all.log" <<'PY'
import re
import sys


def words(path):
    """The ucode words the compiler dumps, in DISK order, row by row."""
    rows = []
    for line in open(path, "r", encoding="utf-8"):
        m = re.match(r"\s*(\d+):((?:\s+[0-9a-fA-F]{8})+)\s*$", line)
        if m:
            rows.append([int(w, 16) for w in m.group(2).split()])
    return rows


def logical(disk_word):
    return ((disk_word >> 16) | ((disk_word & 0xFFFF) << 16)) & 0xFFFFFFFF


MOV = 0x01

# Literal const-block words as the container carries them, read off the
# compiler's own dump rather than derived: a value written by hand here is
# a second implementation of the packer, and would agree with it or not
# for reasons no failure message could explain.
LIT = {0.5: 0x00003F00, 0.25: 0x00003E80, 0.125: 0x00003E00, 1.0: 0x00003F80}


def mask_of(row, what):
    w = logical(row[0])
    if ((w >> 24) & 0x3F) != MOV:
        raise SystemExit(
            "FAIL: %s is not a MOV (opcode 0x%x)" % (what, (w >> 24) & 0x3F)
        )
    return (w >> 9) & 0xF


one_rows = words(sys.argv[1])
all_rows = words(sys.argv[2])

# --- one lane overridden -------------------------------------------------
# MOV R0.yzw, f[TEX0] ; MOV R0.x, {0.5,0,0,0}.x ; the const block.
if len(one_rows) != 3:
    raise SystemExit(
        "FAIL: fp_insert_literal_f must emit the masked base MOV, the "
        "literal override and its const block - three ucode rows, not %d.  "
        "Fewer rows mean the insert was dropped (t_afb4af65): a single "
        "full-width MOV of the varying is exactly what the defect looked "
        "like." % len(one_rows)
    )
masks = [mask_of(one_rows[0], "the base MOV"),
         mask_of(one_rows[1], "the override MOV")]
if masks != [0xE, 0x1]:
    raise SystemExit(
        "FAIL: expected the base MOV masked yzw (0xe) - the lanes the insert "
        "does NOT write - then the override masked x (0x1); got "
        + ", ".join("0x%x" % m for m in masks)
    )
if one_rows[2][0] != LIT[0.5]:
    raise SystemExit(
        "FAIL: the override's const block must hold 0.5 (0x%08x), holds "
        "0x%08x" % (LIT[0.5], one_rows[2][0])
    )

# --- every lane overridden ----------------------------------------------
# No read of the varying at all, so the completeness guard must not refuse.
if len(all_rows) != 8:
    raise SystemExit(
        "FAIL: fp_insert_literal_all_f must emit four overrides and their "
        "four const blocks - eight ucode rows, not %d" % len(all_rows)
    )
masks = [mask_of(all_rows[i], "override %d" % (i // 2)) for i in (0, 2, 4, 6)]
if masks != [0x1, 0x2, 0x4, 0x8]:
    raise SystemExit(
        "FAIL: fp_insert_literal_all_f must write x, y, z then w; got "
        + ", ".join("0x%x" % m for m in masks)
    )
for row, value in zip((1, 3, 5, 7), (0.5, 0.25, 0.125, 1.0)):
    if all_rows[row][0] != LIT[value]:
        raise SystemExit(
            "FAIL: const block %d must hold %s (0x%08x), holds 0x%08x"
            % (row // 2, value, LIT[value], all_rows[row][0])
        )
PY

printf 'lane-insert-literal-test: ok\n'
