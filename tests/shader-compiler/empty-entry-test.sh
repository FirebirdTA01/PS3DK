#!/usr/bin/env bash
# t_38a14d6f: a fragment program with NO EFFECT is legal.
#
# The reference compiler accepts 'void main() {}' (SDK
# samples/tutorial/SpuGraphics/SpuRender/shader/fnop.cg) and emits exactly
# one instruction - four zero words with PROGRAM_END, which on disk reads
# 00010000 00000000 00000000 00000000 - with instructionCount 1 and
# registerCount 1 (every other program declares at least 2).  With no
# parameter table at all the container's parameterArray field is 0, not
# the header size.  Parameters the program never touches are still
# recorded.  We refused all of these with "no instructions emitted".
#
# "No effect" is read off the IR: no returned output, no output store, no
# discard.  An emission that comes out empty for any other program lost
# its code somewhere and stays refused - that is why the gate is not
# simply "the instruction list is empty".
#
# Every accept row pins the WHOLE container, byte for byte, as measured
# from sce-cgc 475 on 2026-09-24.  The two controls pin what must NOT
# change: a discard-only program keeps its KIL and is not the NOP program,
# and the VERTEX profile still refuses an empty entry (the reference
# refuses it too, C6014: required output HPOS not written) with exit
# status exactly 1 and no artifact.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-empty-entry-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

fx="$repo_root/tools/rsx-cg-compiler/tests/shaders"
hex_of() { od -An -tx1 -v "$1" | tr -d ' \n'; }

# The no-parameter NOP program: header, FP header, one NOP+END instruction.
NOP_NO_PARAMS=00001b5c000000060000005000000000000000000000002000000010000000400000000100000000000000000000ffff0000010000000000000000000000000000010000000000000000000000000000

accept_row() {   # accept_row <fixture> <expected hex> <label>
    local stem="${1%.cg}" rc=0
    "$compiler" -p sce_fp_rsx --emit-container "$work/$stem.fpo" "$fx/$1" \
        >"$work/$stem.log" 2>&1 || rc=$?
    [[ "$rc" -eq 0 ]] || { tail -n 3 "$work/$stem.log" >&2; fail "$3: expected exit 0, got $rc"; }
    [[ -s "$work/$stem.fpo" ]] || fail "$3: exit 0 but wrote no container"
    [[ "$(hex_of "$work/$stem.fpo")" == "$2" ]] \
        || fail "$3: container differs from the reference bytes (got $(hex_of "$work/$stem.fpo"))"
}

accept_row fp_empty_entry_f.cg "$NOP_NO_PARAMS" \
    "an empty fragment entry is the one-NOP program"
accept_row fp_empty_deadcode_f.cg "$NOP_NO_PARAMS" \
    "dead local arithmetic leaves the one-NOP program"
accept_row fp_empty_unused_input_f.cg \
    00001b5c000000060000009000000001000000200000006000000010000000800000041800000c9400001005ffffffff0000005a00000000000000000000005000001001000000000000000000000000544558434f4f524430007400000000000000000100000000000000000000ffff0000010000000000000000000000000000010000000000000000000000000000 \
    "an unused varying input is recorded, unreferenced, around the one-NOP program"
accept_row fp_empty_dead_fetch_f.cg \
    00001b5c00000006000000c000000002000000200000009000000010000000b00000042a00000cb800001006ffffffff00000080000000000000000000000000000010010000000000000000000000000000041600000c9400001005ffffffff0000008c000000000000000000000082000010010000000100000000000000007300544558434f4f52443000757600000000000100000000000000000000ffff0000010000000000000000000000000000010000000000000000000000000000 \
    "a texture fetch whose result is never used leaves the one-NOP program"

# Control: a discard IS an effect.  Not the NOP program: more than one
# instruction, and the first instruction is not the bare NOP+END word.
rc=0
"$compiler" -p sce_fp_rsx --emit-container "$work/discard.fpo" \
    "$fx/fp_empty_discard_only_f.cg" >"$work/discard.log" 2>&1 || rc=$?
[[ "$rc" -eq 0 ]] || fail "discard-only control: expected exit 0, got $rc"
d="$(hex_of "$work/discard.fpo")"
[[ "$d" != *00010000000000000000000000000000 ]] \
    || fail "discard-only control: emitted the NOP program - a discard was treated as no effect"
count_hex="$(od -An -tx1 -v -j $(( 0x$(od -An -tx1 -v -j 20 -N 4 "$work/discard.fpo" | tr -d ' \n') )) -N 4 "$work/discard.fpo" | tr -d ' \n')"
[[ "$count_hex" != 00000001 ]] \
    || fail "discard-only control: instructionCount is 1 - a discard was treated as no effect"

# Controls: programs that DO have an effect but leave no output store in
# our IR must stay refused, never become the NOP program.  An out-STRUCT
# parameter's member stores and a missing return both produce no
# StoreOutput today; an IR-only "no effect" test accepted the first as a
# NOP (a program that should draw, drawing nothing) - found by
# struct_field_update_check.py.  An unwritten plain out param IS the NOP
# program on the reference, but we refuse it rather than open that door.
refuse_row() {   # refuse_row <fixture> <label>
    local stem="${1%.cg}" rc=0
    "$compiler" -p sce_fp_rsx --emit-container "$work/$stem.fpo" "$fx/$1" \
        >"$work/$stem.log" 2>&1 || rc=$?
    [[ "$rc" -eq 1 ]] || fail "$2: expected exit 1 (a refusal), got $rc"
    [[ ! -e "$work/$stem.fpo" ]] || fail "$2: refused but still wrote a container"
}
refuse_row fp_empty_out_struct_refuse_f.cg "an out-struct store is an effect"
refuse_row fp_empty_missing_return_refuse_f.cg "a non-void entry with no return is not the NOP program"
refuse_row fp_empty_unwritten_out_refuse_f.cg "an entry with an out parameter is not treated as effect-free"

# Control: the vertex profile refuses an empty entry, as the reference does.
rc=0
"$compiler" -p sce_vp_rsx --emit-container "$work/vp.vpo" \
    "$fx/vp_empty_entry_refuse_v.cg" >"$work/vp.log" 2>&1 || rc=$?
[[ "$rc" -eq 1 ]] || fail "vertex empty entry: expected exit 1 (a refusal), got $rc"
[[ ! -e "$work/vp.vpo" ]] || fail "vertex empty entry: refused but still wrote a container"

echo "empty-entry: PASS"
