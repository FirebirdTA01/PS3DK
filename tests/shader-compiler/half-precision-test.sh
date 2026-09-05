#!/usr/bin/env bash
# Half precision on the general path (t_b8bb521f).
#
# Two fixtures, one per half of the item, each reduced from a corpus
# shader that NO path compiled - th06_mod and th06_add, the last two of
# the twelve discard witnesses, both refusing for reasons that had nothing
# to do with their discards:
#
#   fp_half_cast_f          "unsupported IR op ftoh"
#   fp_insert_undef_base_f  "operand %n could not be resolved"
#
# NV40 has no conversion opcode - the conversion IS the register file the
# value lands in - so a float-to-half cast is a MOV with an fp16
# destination.  That is asserted on the DECODED precision field rather
# than on the compile succeeding, because "it compiled" would pass on a
# lowering that dropped the cast entirely.
#
# The second fixture's assertion is that ALL FOUR output lanes are
# written.  Its `half4 c;` has no initialiser, so every lane comes from an
# insert; a lowering that lost the chain writes some of them and leaves
# the rest holding whatever the register had, which is exactly what the
# old bail produced one step later.
#
# GENERAL path only.  The default path refuses both shapes for its own
# reasons and this item does not touch it; asserting a default column
# would be asserting the matcher's coverage rather than half precision.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-half-precision.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"
for f in fp_half_cast_f.cg fp_insert_undef_base_f.cg; do
    [[ -f "$shaders/$f" ]] || fail "fixture missing: $shaders/$f"
done

dump() {   # $1 stem
    local rc=0
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" \
            -p sce_fp_rsx "$shaders/$1.cg"
    ) >"$work/$1.dump" 2>&1 || rc=$?
    [[ "$rc" -eq 124 ]] && fail "$1 timed out"
    if [[ "$rc" -ne 0 ]]; then
        tail -n 20 "$work/$1.dump" >&2
        fail "$1 did not compile on the general path.  Both shapes are
what kept th06_mod and th06_add out of every sweep (t_b8bb521f)."
    fi
    return 0
}

dump fp_half_cast_f
dump fp_insert_undef_base_f

( cd "$repo_root/tests/shader-compiler" && python3 - \
    "$work/fp_half_cast_f.dump" "$work/fp_insert_undef_base_f.dump" <<'PY'
import sys

from ucode_decode import decode

cast_dump, insert_dump = sys.argv[1], sys.argv[2]

FP16 = 1

# 1.  The cast must reach the ucode as an fp16 DESTINATION.  A lowering
#     that dropped it would compile and compute the same value at fp32,
#     so "it compiled" is not the assertion.
half_dsts = [d for d in decode(cast_dump)
             if d["opcode"] != 0 and d["prec"] == FP16]
if not half_dsts:
    raise SystemExit(
        "FAIL: fp_half_cast_f emitted no fp16-precision instruction.  On "
        "NV40 the conversion IS the register file the value lands in, so "
        "a float-to-half cast must appear as a MOV with an fp16 "
        "destination (t_b8bb521f).")

# 2.  Every lane of the output must be written.  `half4 c;` has no
#     initialiser, so all four come from inserts.
lanes = 0
for d in decode(insert_dump):
    if d["opcode"] == 0 or d["none"]:
        continue
    if d["dst"] == 0:                 # the colour output is register 0
        lanes |= d["mask"]
if lanes != 0xF:
    raise SystemExit(
        "FAIL: fp_insert_undef_base_f writes output lanes 0x%X, not 0xF.  "
        "Its vector is built entirely by lane writes over an "
        "uninitialised base; a lane missing here is a lost insert, and "
        "the register keeps whatever it held (t_b8bb521f)." % lanes)

print("half-precision: the cast reaches the ucode as fp16, and a vector "
      "built over an undefined base writes all four lanes")
PY
)

printf 'PASS: half-precision-test\n'
