#!/usr/bin/env bash
# t_99b29225: a VERTEX array uniform indexed at RUN TIME.  Commit B of the
# array-uniform slice (commit A, 5896ac2a, laid constant-index elements out
# one at a time and refused every run-time index by name).
#
# What the reference emits, measured on sce-cgc and pinned here as literal
# expectations (the reference cannot run in CI):
#   - the array takes a CONTIGUOUS block of constant registers, all N
#     elements referenced, consecutive resIndex in element order, taken at
#     the array's place in the descending walk (u_bones[4] alone: c464..c467;
#     after u_scale=c467 and u_a[1]=c466, u_b[3] is c463..c465).  A constant
#     index into the same array reads its element inside the block as an
#     ordinary const;
#   - ONE ARL per distinct index VALUE, loading a lane of the address
#     register from the index's own source and swizzle; a cast that only the
#     index consumes becomes the ARL itself (ARL floors); a computed index
#     goes through a temp.  Lanes are handed out in order of first use across
#     A0 then A1, and index values read from ONE register share ONE ARL whose
#     unwritten lanes replicate the first value's component (idx.x then idx.y
#     -> ARL A0.xy, v.xyxx; idx.y then idx.x -> v.yxyy);
#   - every relative read names the block base in CONST_SRC with INDEX_CONST
#     set and ADDR_SWZ naming the lane (A1 via ADDR_REG_SELECT_1);
#   - scalar, float3, half4 and parameter arrays take the same block; a half
#     element is recorded as float, as commit A found for constant indices.
#
# Every accept row is RED on the parent, which refuses each of these sources
# with "run-time index into array".  The refusals keep the slice honest: an
# integer-arithmetic index (int(idx) + 1) needs the vertex float-to-int
# lowering, which is still deferred; a float-typed index is the same
# frontend gap commit A pinned on the fragment side (the reference accepts
# both); nine distinct index values need lane REUSE over the eight address
# lanes, which the reference performs and this slice does not; a fragment
# run-time index stays refused (array-uniform-test.sh, C6013).
#
# With PS3_REF_CG_COMPILER set (the reference's executable, never named in
# the tree), the accept rows are also compiled by the reference and
# byte-compared; the rows listed in BYTE_IDENTICAL must match, the rest
# report their status.  Without it the reference rows are simply absent.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-vp-array-dynamic.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT
shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"
decoder="$repo_root/tests/shader-compiler/vp_words.py"

compile() {  # <compiler> <stem> [profile] -> exit status; container at $work/<stem>.bin
    local cc="$1" stem="$2" profile="${3:-sce_vp_rsx}" rc=0
    rm -f "$work/$stem.bin"
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-20s}" "$cc" \
            -p "$profile" --emit-container "$work/$stem.bin" "$shaders/$stem.cg"
    ) >"$work/$stem.log" 2>&1 || rc=$?
    [[ "$rc" -eq 124 ]] && fail "$stem timed out"
    printf '%s' "$rc"
}
accept() {  # <stem>: exit 0, a container, its parameter table and words decoded
    local stem="$1" rc
    rc="$(compile "$compiler" "$stem")"
    [[ "$rc" -eq 0 ]] || { tail -n 3 "$work/$stem.log" >&2; fail "$stem exited $rc, expected 0"; }
    [[ -s "$work/$stem.bin" ]] || fail "$stem compiled but wrote no container"
    params "$stem" > "$work/$stem.params"
    python3 "$decoder" "$work/$stem.bin" > "$work/$stem.words" \
        || fail "$stem: the container's vertex words could not be decoded"
}

# params <stem>: the parameter table in the WRITER's column order (48-byte
# records: type res var resIndex name defaultValue embeddedConstants
# semantic direction paramno isReferenced isShared).
params() {
    python3 - "$work/$1.bin" <<'PY'
import struct, sys
b = open(sys.argv[1], 'rb').read()
_, _, _, count, table, _, _, _ = struct.unpack_from('>8I', b, 0)
def cstr(off):
    if off == 0 or off >= len(b): return ''
    return b[off:b.index(b'\0', off)].decode('latin1')
for i in range(count):
    typ, res, var, resindex, name, _, emb, _, _, paramno, isref, _ = \
        struct.unpack_from('>12I', b, table + i * 48)
    print('%s type=%d paramno=%s ref=%d resIndex=%s' % (
        cstr(name), typ, 'none' if paramno == 0xffffffff else str(paramno), isref,
        'none' if resindex == 0xffffffff else str(resindex)))
PY
}
expect_record() {  # <stem> <regex>: one full parameter line matches
    grep -qE "^$2\$" "$work/$1.params" \
        || { cat "$work/$1.params" >&2; fail "$1: no parameter record matching '$2'"; }
}
expect_block() {  # <stem> <name> <count> <base> <type> [paramno]: all N referenced, consecutive
    local stem="$1" name="$2" count="$3" base="$4" type="$5" pno="${6:-none}" k
    for ((k = 0; k < count; k++)); do
        expect_record "$stem" "${name//[/\\[}\[$k\] type=$type paramno=$pno ref=1 resIndex=$((base + k))"
    done
    local got; got="$(grep -oE "^${name//[/\\[}\[[0-9]+\]" "$work/$stem.params" | tr '\n' ' ' || true)"
    local want=""; for ((k = 0; k < count; k++)); do want+="${name}[$k] "; done
    [[ "$got" == "$want" ]] || { cat "$work/$stem.params" >&2; fail "$stem: elements of $name are '$got', expected '$want'"; }
}
expect_word() {  # <stem> <regex>: one decoded instruction line matches
    grep -qE "$2" "$work/$1.words" \
        || { cat "$work/$1.words" >&2; fail "$1: no instruction matching '$2'"; }
}
expect_count() {  # <stem> <n>: exactly n instructions
    local n; n="$(wc -l < "$work/$1.words" | tr -d ' ')"
    [[ "$n" -eq "$2" ]] || { cat "$work/$1.words" >&2; fail "$1: $n instructions, expected $2"; }
}
count_words() {  # <stem> <regex> -> how many instruction lines match
    grep -cE "$2" "$work/$1.words" || true
}
# The store to an output that reads the block through the named lane.
REL() { printf 'dst=o%s .*C\\[A%s\\+%s\\]' "$1" "$2" "$3"; }
expect_regs() {  # <stem> <n>: the vertex subtype's registerCount (temps the program uses)
    local got; got="$(python3 - "$work/$1.bin" <<'PYR'
import struct, sys
b = open(sys.argv[1], 'rb').read()
prog = struct.unpack_from('>8I', b, 0)[5]
print(struct.unpack_from('>6I', b, prog)[2])
PYR
)"
    [[ "$got" -eq "$2" ]] || fail "$1: registerCount $got, expected $2 (an address register is not a temp)"
}

# ---------------------------------------------------------------------------
# THE FOUR SHAPES MEASURED ON THE CARD.
accept vp_array_uniform_dynamic_v
expect_block vp_array_uniform_dynamic_v u_bones 4 464 1048
expect_count vp_array_uniform_dynamic_v 2
expect_word vp_array_uniform_dynamic_v '^0 ARL dst=A0 mask=x src0=IN9\.xxxx '
expect_word vp_array_uniform_dynamic_v "^1 ADD $(REL 0 '0\.x' 464)\.xyzw"
expect_regs vp_array_uniform_dynamic_v 1
printf '  %-44s block c464..c467, ARL A0.x <- idx, ADD reads c[A0.x+464]\n' "u_bones[int(idx)]"

# A constant index INSIDE a dynamic array reads its element in the block.
accept vp_array_uniform_mixed_v
expect_block vp_array_uniform_mixed_v u_bones 4 464 1048
expect_count vp_array_uniform_mixed_v 3
expect_word vp_array_uniform_mixed_v '^0 ARL dst=A0 mask=x src0=IN9\.xxxx '
expect_word vp_array_uniform_mixed_v '^[0-9]+ ADD dst=R[0-9]+ .*[= ]C465\.xyzw'
expect_word vp_array_uniform_mixed_v "$(REL 0 '0\.x' 464)"
printf '  %-44s u_bones[1] = c465 inside the block\n' "u_bones[1] + u_bones[int(idx)]"

# Two lanes of one index register: ONE ARL, mask xy, swizzle xyxx; the
# reads name A0.x and A0.y.
accept vp_array_uniform_consecutive_v
expect_block vp_array_uniform_consecutive_v u_bones 4 464 1048
expect_count vp_array_uniform_consecutive_v 3
expect_word vp_array_uniform_consecutive_v '^0 ARL dst=A0 mask=xy src0=IN9\.xyxx '
expect_word vp_array_uniform_consecutive_v "^[0-9]+ ADD $(REL 0 '0\.x' 464)"
expect_word vp_array_uniform_consecutive_v "^[0-9]+ MOV $(REL 1 '0\.y' 464)"
[[ "$(count_words vp_array_uniform_consecutive_v ' ARL ')" -eq 1 ]] || fail "consecutive: expected exactly one ARL"
printf '  %-44s one ARL A0.xy <- idx.xyxx\n' "u_bones[int(idx.x)], u_bones[int(idx.y)]"

# Beside a scalar and a constant-index array: the block takes its place in
# the descending walk, and the constant-index array keeps commit A's shape.
accept vp_array_uniform_two_arrays_v
expect_record vp_array_uniform_two_arrays_v 'u_scale type=1048 paramno=none ref=1 resIndex=467'
expect_record vp_array_uniform_two_arrays_v 'u_a\[0\] type=1048 paramno=none ref=0 resIndex=none'
expect_record vp_array_uniform_two_arrays_v 'u_a\[1\] type=1048 paramno=none ref=1 resIndex=466'
expect_block vp_array_uniform_two_arrays_v u_b 3 463 1048
expect_count vp_array_uniform_two_arrays_v 4
expect_word vp_array_uniform_two_arrays_v "$(REL 0 '0\.x' 463)"
printf '  %-44s u_scale c467, u_a[1] c466, u_b block c463..c465\n' "two arrays, one dynamic"

# ---------------------------------------------------------------------------
# WHICH LANE THE ARL WRITES AND WHICH COMPONENT IT READS.
accept vp_array_uniform_dyn_lane_y_v
expect_word vp_array_uniform_dyn_lane_y_v '^0 ARL dst=A0 mask=x src0=IN9\.yyyy '
expect_word vp_array_uniform_dyn_lane_y_v "$(REL 0 '0\.x' 464)"
accept vp_array_uniform_dyn_lane_w_v
expect_word vp_array_uniform_dyn_lane_w_v '^0 ARL dst=A0 mask=x src0=IN9\.wwww '
expect_word vp_array_uniform_dyn_lane_w_v "$(REL 0 '0\.x' 464)"
printf '  %-44s A0.x <- idx.y / idx.w (lane by first use, not by source lane)\n' "int(idx.y), int(idx.w)"

accept vp_array_uniform_dyn_yx_order_v
expect_count vp_array_uniform_dyn_yx_order_v 3
expect_word vp_array_uniform_dyn_yx_order_v '^0 ARL dst=A0 mask=xy src0=IN9\.yxyy '
expect_word vp_array_uniform_dyn_yx_order_v "$(REL 0 '0\.x' 464)"
expect_word vp_array_uniform_dyn_yx_order_v "$(REL 1 '0\.y' 464)"
printf '  %-44s one ARL A0.xy <- idx.yxyy\n' "int(idx.y) then int(idx.x)"

# Two index REGISTERS: two ARLs (one input per instruction), lanes x then y.
accept vp_array_uniform_dyn_two_values_v
expect_count vp_array_uniform_dyn_two_values_v 4
expect_word vp_array_uniform_dyn_two_values_v '^[0-9]+ ARL dst=A0 mask=x src0=IN9\.xxxx '
expect_word vp_array_uniform_dyn_two_values_v '^[0-9]+ ARL dst=A0 mask=y src0=IN10\.xxxx '
expect_word vp_array_uniform_dyn_two_values_v 'C\[A0\.x\+464\]'
expect_word vp_array_uniform_dyn_two_values_v "$(REL 0 '0\.y' 464)"
printf '  %-44s ARL A0.x <- idx, ARL A0.y <- jdx\n' "int(idx), int(jdx)"

# Five values: the fifth takes A1.x (ADDR_REG_SELECT_1 on its read).
accept vp_array_uniform_dyn_five_values_v
expect_count vp_array_uniform_dyn_five_values_v 7
expect_word vp_array_uniform_dyn_five_values_v '^[0-9]+ ARL dst=A0 mask=xyzw src0=IN9\.xyzw '
expect_word vp_array_uniform_dyn_five_values_v '^[0-9]+ ARL dst=A1 mask=x src0=IN10\.xxxx '
for lane in x y z w; do expect_word vp_array_uniform_dyn_five_values_v "C\\[A0\\.$lane\\+464\\]"; done
expect_word vp_array_uniform_dyn_five_values_v "$(REL 0 '1\.x' 464)"
expect_regs vp_array_uniform_dyn_five_values_v 1
printf '  %-44s A0.xyzw then A1.x\n' "five index values"

# ---------------------------------------------------------------------------
# WHAT FEEDS THE ARL.
accept vp_array_uniform_dyn_int_input_v
expect_count vp_array_uniform_dyn_int_input_v 2
expect_word vp_array_uniform_dyn_int_input_v '^0 ARL dst=A0 mask=x src0=IN9\.xxxx '
accept vp_array_uniform_dyn_via_local_v
cmp -s "$work/vp_array_uniform_dyn_via_local_v.bin" "$work/vp_array_uniform_dynamic_v.bin" \
    || fail "int i = int(idx); u_bones[i] did not compile to the bytes of u_bones[int(idx)]"
printf '  %-44s int input direct; local == inline cast\n' "int idx : TEXCOORD1; int i = int(idx)"

# A computed index is a temp: MUL into a lane, ARL from that lane.
accept vp_array_uniform_dyn_computed_v
expect_count vp_array_uniform_dyn_computed_v 3
expect_word vp_array_uniform_dyn_computed_v '^[0-9]+ MUL dst=R0 mask=x src0=IN9\.xxxx src1=C[0-9]+\.xxxx'
expect_word vp_array_uniform_dyn_computed_v '^[0-9]+ ARL dst=A0 mask=x src0=R0\.xxxx '
expect_word vp_array_uniform_dyn_computed_v "$(REL 0 '0\.x' 464)"
printf '  %-44s MUL R0.x, ARL A0.x <- R0.x\n' "int(idx * 2.0)"

# ---------------------------------------------------------------------------
# ELEMENT TYPES: half4 recorded as float4, float3 as 1047 with its swizzle
# composed through the relative read, scalars one register each (8 of them).
accept vp_array_uniform_dyn_half4_v
expect_block vp_array_uniform_dyn_half4_v u_hb 4 464 1048
expect_word vp_array_uniform_dyn_half4_v "$(REL 0 '0\.x' 464)\.xyzw"
accept vp_array_uniform_dyn_float3_v
expect_block vp_array_uniform_dyn_float3_v u_v 4 464 1047
expect_word vp_array_uniform_dyn_float3_v "$(REL 0 '0\.x' 464)\.xyzx"
accept vp_array_uniform_dyn_scalar_v
expect_block vp_array_uniform_dyn_scalar_v u_w 8 460 1045
expect_count vp_array_uniform_dyn_scalar_v 2
expect_word vp_array_uniform_dyn_scalar_v "^1 MUL $(REL 0 '0\.x' 460)\.xxxx"
printf '  %-44s half4 -> 1048, float3 .xyzx, float[8] c460..c467 .xxxx\n' "element types"

# A PARAMETER array: every element carries the parameter's ordinal.
accept vp_array_uniform_dyn_param_v
expect_block vp_array_uniform_dyn_param_v u_bones 4 464 1048 2
expect_word vp_array_uniform_dyn_param_v '^0 ARL dst=A0 mask=x src0=IN9\.xxxx '
printf '  %-44s paramno 2 on all four\n' "uniform float4 u_bones[4] parameter"

# ---------------------------------------------------------------------------
# THE ADDRESS REGISTER IN THE SCHEDULE.  The same element read twice through
# one lane: one ARL, two relative reads.  Independent work beside the ARL:
# every relative read still follows its ARL.
accept vp_array_uniform_dyn_same_twice_v
[[ "$(count_words vp_array_uniform_dyn_same_twice_v ' ARL ')" -eq 1 ]] || fail "same_twice: expected exactly one ARL"
[[ "$(count_words vp_array_uniform_dyn_same_twice_v 'C\[A0\.x\+464\]')" -eq 2 ]] || { cat "$work/vp_array_uniform_dyn_same_twice_v.words" >&2; fail "same_twice: expected two relative reads"; }
accept vp_array_uniform_dyn_work_between_v
expect_count vp_array_uniform_dyn_work_between_v 3
expect_word vp_array_uniform_dyn_work_between_v '^[0-9]+ MAX dst=R0 '
expect_word vp_array_uniform_dyn_work_between_v "^[0-9]+ ADD $(REL 0 '0\.x' 464)"
arl_at="$(grep -E ' ARL ' "$work/vp_array_uniform_dyn_work_between_v.words" | cut -d' ' -f1)"
read_at="$(grep -E 'C\[A0\.x\+464\]' "$work/vp_array_uniform_dyn_work_between_v.words" | cut -d' ' -f1)"
[[ -n "$arl_at" && -n "$read_at" && "$arl_at" -lt "$read_at" ]] || fail "work_between: ARL at '$arl_at' must precede the relative read at '$read_at'"
printf '  %-44s one ARL; ARL precedes its read across independent work\n' "same element twice; MAX beside the ARL"

# The relative read HEADS a chain long enough to earn the scheduler's demand
# priority: this is the row that sees the address-register RAW edge (with
# the edge removed, the MUL is picked before its ARL).
accept vp_array_uniform_dyn_chain_v
arl_at="$(grep -E ' ARL ' "$work/vp_array_uniform_dyn_chain_v.words" | cut -d' ' -f1)"
read_at="$(grep -E 'C\[A0\.x\+464\]' "$work/vp_array_uniform_dyn_chain_v.words" | cut -d' ' -f1)"
[[ -n "$arl_at" && -n "$read_at" && "$arl_at" -lt "$read_at" ]] || { cat "$work/vp_array_uniform_dyn_chain_v.words" >&2; fail "chain: ARL at '$arl_at' must precede the relative read at '$read_at'"; }
printf '  %-44s ARL precedes a read that heads a chain
' "chain after the read"

# The rig row (tests/regression/shader-differential/vp-pairs.txt): two
# computed indices from one attribute's lanes are two temps, so two ARLs.
accept vp_array_uniform_dyn_rig_v
expect_block vp_array_uniform_dyn_rig_v u_bones 4 464 1048
[[ "$(count_words vp_array_uniform_dyn_rig_v ' ARL dst=A0 mask=[xy] src0=R[0-9]+\.')" -eq 2 ]] || { cat "$work/vp_array_uniform_dyn_rig_v.words" >&2; fail "rig row: expected two ARLs from temps"; }
expect_word vp_array_uniform_dyn_rig_v 'C\[A0\.x\+464\]'
expect_word vp_array_uniform_dyn_rig_v 'C\[A0\.y\+464\]'
printf '  %-44s two temp indices, two ARLs
' "rig row"

# A CONSTANT-PROPAGATED index (`int i = 1; u[i]`, `float i = 1; u[int(i)]`)
# is a constant index to the reference: per-element layout, no ARL, the
# same on the fragment side.  Beside a real run-time index the array is a
# block and the folded read is its element (review: codex's twins, which
# the first sha read as ONE lane for two literals - u[1] + u[1]).
accept vp_array_uniform_dyn_local_const_v
expect_record vp_array_uniform_dyn_local_const_v 'u\[0\] type=1048 paramno=none ref=0 resIndex=none'
expect_record vp_array_uniform_dyn_local_const_v 'u\[1\] type=1048 paramno=none ref=1 resIndex=467'
expect_record vp_array_uniform_dyn_local_const_v 'u\[2\] type=1048 paramno=none ref=1 resIndex=466'
[[ "$(count_words vp_array_uniform_dyn_local_const_v ' ARL ')" -eq 0 ]] || { cat "$work/vp_array_uniform_dyn_local_const_v.words" >&2; fail "local_const: a constant-propagated index must not emit an ARL"; }
expect_word vp_array_uniform_dyn_local_const_v '[= ]C467\.xyzw'
expect_word vp_array_uniform_dyn_local_const_v '[= ]C466\.xyzw'
accept vp_array_uniform_dyn_local_const_float_v
cmp -s "$work/vp_array_uniform_dyn_local_const_float_v.bin" "$work/vp_array_uniform_dyn_local_const_v.bin"     || fail "float i = 1; u[int(i)] did not compile to the bytes of int i = 1; u[i]"
accept vp_array_uniform_dyn_local_const_mixed_v
expect_block vp_array_uniform_dyn_local_const_mixed_v u 4 464 1048
expect_word vp_array_uniform_dyn_local_const_mixed_v '[= ]C465\.xyzw'
expect_word vp_array_uniform_dyn_local_const_mixed_v 'C\[A0\.x\+464\]'
[[ "$(count_words vp_array_uniform_dyn_local_const_mixed_v ' ARL ')" -eq 1 ]] || fail "local_const_mixed: expected exactly one ARL"
printf '  %-44s per element c467/c466, no ARL; beside a run-time index: block
' "int i = 1; u[i]"

# The same on the fragment side: the folded index is a constant element -
# compiled under the FRAGMENT profile (review: codex - the first version sent
# this source through the vertex-profile helper and asserted only rc), the
# container's profile word checked, u[1] and u[2] referenced with their own
# inline constants and u[0]/u[3] declared without.
rc="$(compile "$compiler" fp_array_uniform_local_const_f sce_fp_rsx)"; [[ "$rc" -eq 0 && -s "$work/fp_array_uniform_local_const_f.bin" ]] || { tail -n 3 "$work/fp_array_uniform_local_const_f.log" >&2; fail "fp_array_uniform_local_const_f exited $rc, expected 0"; }
python3 - "$work/fp_array_uniform_local_const_f.bin" <<'PYF' || fail "fp_array_uniform_local_const_f: not a fragment container with u[1] and u[2] referenced (each with an inline constant) and u[0]/u[3] unreferenced"
import struct, sys
b = open(sys.argv[1], 'rb').read()
profile, _, _, count, table, _, _, _ = struct.unpack_from('>8I', b, 0)
assert profile == 7004, ('profile word', profile)
def cstr(off): return b[off:b.index(b'\0', off)].decode('latin1') if off else ''
got = {}
for i in range(count):
    typ, res, var, resindex, name, _, emb, _, _, paramno, isref, _ = struct.unpack_from('>12I', b, table + i * 48)
    if cstr(name).startswith('u['): got[cstr(name)] = (isref, emb != 0)
assert got == {'u[0]': (0, False), 'u[1]': (1, True), 'u[2]': (1, True), 'u[3]': (0, False)}, got
PYF
printf '  %-44s fragment container, u[1]/u[2] referenced
' "FP int i = 1; u[i]"

# A temp index REWRITTEN in place between two reads is two index values:
# the second read must name a different lane from the first (review:
# codex - the first sha reused the lane because the vreg and component
# matched, and read u[old v.x] twice).
accept vp_array_uniform_dyn_rewrite_v
expect_word vp_array_uniform_dyn_rewrite_v 'C\[A0\.x\+464\]'
expect_word vp_array_uniform_dyn_rewrite_v 'C\[A0\.y\+464\]'
accept vp_array_uniform_dyn_same_expr_twice_v
[[ "$(count_words vp_array_uniform_dyn_same_expr_twice_v ' ARL ')" -eq 1 ]] || fail "same_expr_twice: expected exactly one ARL"
[[ "$(count_words vp_array_uniform_dyn_same_expr_twice_v 'C\[A0\.x\+464\]')" -eq 2 ]] || { cat "$work/vp_array_uniform_dyn_same_expr_twice_v.words" >&2; fail "same_expr_twice: expected two reads through A0.x"; }
printf '  %-44s rewrite = two lanes; same expression twice = one
' "v.x = v.y between reads"

# A FLOAT-TYPED index (t_050bebce).  The reference accepts it on both
# profiles with int() semantics: a run-time float or half index is the same
# ARL as int(idx) (byte-identical containers), a constant truncates toward
# zero (1.7 -> 1, -0.5 -> 0, 3.9 -> 3; 4.0 refuses C1068), an UNSIGNED cast of
# 2^32 wraps to element 0 (a signed one is INT_MIN and out of bounds), and a
# float literal is SINGLE precision before any conversion, so 4294967297.0
# under an unsigned int or unsigned char cast is element 0 too (review: codex),
# a local float
# constant folds the same way, and an expression goes through a temp.
accept vp_array_uniform_dyn_float_index_v
cmp -s "$work/vp_array_uniform_dyn_float_index_v.bin" "$work/vp_array_uniform_dynamic_v.bin"     || fail "u_bones[idx] with a float idx did not compile to the bytes of u_bones[int(idx)]"
accept vp_array_uniform_dyn_half_index_v
cmp -s "$work/vp_array_uniform_dyn_half_index_v.bin" "$work/vp_array_uniform_dynamic_v.bin"     || fail "u_bones[idx] with a half idx did not compile to the bytes of u_bones[int(idx)]"
for pair in "vp_array_uniform_dyn_float_const_v:1" "vp_array_uniform_dyn_float_neg_v:0" "vp_array_uniform_dyn_float_39_v:3" "vp_array_uniform_dyn_float_local_v:2" "vp_array_uniform_dyn_uint_cast_wrap_v:0" "vp_array_uniform_dyn_uint_cast_wrap1_v:0" "vp_array_uniform_dyn_uchar_cast_wrap_v:0" "vp_array_uniform_dyn_float_near0_v:1" "vp_array_uniform_dyn_float_near1_v:2" "vp_array_uniform_dyn_float_near2_v:3"; do
    stem="${pair%%:*}"; k="${pair##*:}"
    accept "$stem"
    expect_record "$stem" "u\[$k\] type=1048 paramno=none ref=1 resIndex=467"
    [[ "$(count_words "$stem" ' ARL ')" -eq 0 ]] || { cat "$work/$stem.words" >&2; fail "$stem: a constant float index must not emit an ARL"; }
    for j in 0 1 2 3; do
        [[ "$j" -eq "$k" ]] || expect_record "$stem" "u\[$j\] type=1048 paramno=none ref=0 resIndex=none"
    done
done
accept vp_array_uniform_dyn_float_expr_v
expect_word vp_array_uniform_dyn_float_expr_v '^[0-9]+ MUL dst=R0 mask=x src0=IN9\.xxxx '
expect_word vp_array_uniform_dyn_float_expr_v '^[0-9]+ ARL dst=A0 mask=x src0=R0\.xxxx '
expect_word vp_array_uniform_dyn_float_expr_v "$(REL 0 '0\.x' 464)"
printf '  %-44s float/half idx == int(idx); 1.7 -> [1], -0.5 -> [0], 3.9 -> [3], local 2.5 -> [2]
' "float-typed index"

# ---------------------------------------------------------------------------
# REFUSALS: exit exactly 1, no artifact, the named body.
check_refusal() {  # <compiler> <stem> <body> -> problem or nothing
    local cc="$1" stem="$2" body="$3" rc
    rc="$(compile "$cc" "$stem")"
    if [[ "$rc" -ne 1 ]]; then printf 'exited %s, expected exactly 1' "$rc"; return 0; fi
    if [[ -e "$work/$stem.bin" ]]; then printf 'refused but left an output artifact'; return 0; fi
    if ! grep -qF -- "$body" "$work/$stem.log"; then printf "stderr does not contain '%s'" "$body"; return 0; fi
}
refuse() {  # <label> <stem> <body>
    local problem; problem="$(check_refusal "$compiler" "$2" "$3")"
    [[ -z "$problem" ]] || { head -n 4 "$work/$2.log" >&2; fail "$1: $problem"; }
    printf '  %-44s refused\n' "$1"
}
refuse "int(idx) + 1 (VP float-to-int deferred)" vp_array_uniform_dyn_arith_refuse_v "float-to-int"
refuse "float index out of range (C1068 in the reference)" vp_array_uniform_dyn_float_oob_refuse_v "out of bounds"
# A CONSTANT floating expression beyond a bare literal or a floating cast of
# a constant is REFUSED BY NAME until the typed constant evaluator serves
# this path (t_65e1b7fa follow-up): the reference evaluates it in float and
# truncates ONCE on the result - u[1.7 * 2.0] is element 3 (leaf truncation
# reads 2), u[((float)3 / 2) * 2] is element 3 (leaf truncation reads 2, and
# there is no float LITERAL in it), u[(int)(bool)(float)0.5] is element 1
# (the fraction is truthy; leaf truncation reads 0).  The last two were
# found in review (codex) on a candidate that accepted them WRONG; a
# refusal is the honest interim, and these rows flip to accept rows with
# element assertions when the evaluator lands.
refuse "u[1.7 * 2.0] (reference: element 3)"        vp_array_uniform_dyn_float_arith_v      "floating value"
refuse "u[((float)3 / 2) * 2] (reference: 3)"        vp_array_uniform_dyn_float_cast_arith_v "floating value"
refuse "u[(int)(bool)(float)0.5] (reference: 1)"     vp_array_uniform_dyn_float_cast_bool_v  "floating value"
# A FIXED cast clamps to [-2, 2 - 2^-10] before it is read - the reference
# reads u[(fixed)3] as element 1 - and that conversion is the typed
# evaluator's, so the cast refuses by name here (review: codex).  An integral
# cast of an out-of-range float literal folds to INT_MIN (the x86 indefinite
# value the reference uses, measured on t_65e1b7fa) and is out of bounds.
refuse "u[(fixed)3] (reference: element 1)"           vp_array_uniform_dyn_fixed_cast_refuse_v "floating value"
refuse "u[(int)4294967296.0] (C1068 in the reference)" vp_array_uniform_dyn_int_cast_oob_refuse_v "out of bounds"
# The float32 rule on bare literals, directly: 0.9999999999 is 1.0f before
# it is read (element 1), and 3.9999999999 is 4.0f - out of bounds (claude's
# rows).  The unsigned wrap has a bound: 2^32 + 512 (exactly representable
# in fp32) narrows to 512 and is out of bounds, which separates "int64, then
# narrow to 32, then the ordinary bounds check" from "wrap modulo the array".
refuse "u[3.9999999999] (4.0f: C1068 in the reference)" vp_array_uniform_dyn_float_near3_v "out of bounds"
refuse "u[(unsigned int)4294967808.0] (C1068)"        vp_array_uniform_dyn_uint_cast_oob_refuse_v "out of bounds"
refuse "nine index values (lane reuse not lowered)" vp_array_uniform_dyn_nine_values_refuse_v "address register lanes"
refuse "int i = 4; u[i] (C1068 in the reference)" vp_array_uniform_dyn_local_oob_refuse_v "out of bounds"
refuse "int i = -1; u[i]"                    vp_array_uniform_dyn_local_neg_refuse_v "out of bounds"

# SELF-CHECK: an exit-1 stub that leaves an EMPTY artifact, and one that
# refuses for an unrelated reason, must both be rejected.
stub="$work/stub"
{ echo '#!/usr/bin/env bash'; echo 'for a in "$@"; do case "$prev" in --emit-container) : > "$a";; esac; prev="$a"; done'; echo 'echo "x:1:1: error: float-to-int" >&2'; echo 'exit 1'; } > "$stub"; chmod +x "$stub"
problem="$(check_refusal "$stub" vp_array_uniform_dyn_arith_refuse_v "float-to-int")"
[[ "$problem" == *"left an output artifact"* ]] || fail "self-check: an empty-artifact stub was not rejected ('$problem')"
{ echo '#!/usr/bin/env bash'; echo 'echo "x:1:1: error: unrelated stage failure" >&2'; echo 'exit 1'; } > "$stub"
problem="$(check_refusal "$stub" vp_array_uniform_dyn_arith_refuse_v "float-to-int")"
[[ "$problem" == *"does not contain"* ]] || fail "self-check: an unrelated-refusal stub was not rejected ('$problem')"
printf '  %-44s ok\n' "self-check: two stubs rejected"

# ---------------------------------------------------------------------------
# OPTIONAL REFERENCE ROWS.  Byte identity is the strongest predicate and the
# one the decoded fields above approximate; it is measured here only when
# the reference is present.  A row NOT in BYTE_IDENTICAL differs today for a
# reason outside this slice (operand order of a commutative op with a plain
# const, ARL placement in the schedule, a MAD fusion the constant-index twin
# loses too, lane-level dead-code elimination) and is reported, not judged.
BYTE_IDENTICAL=(vp_array_uniform_dynamic_v vp_array_uniform_consecutive_v
    vp_array_uniform_dyn_lane_y_v vp_array_uniform_dyn_lane_w_v
    vp_array_uniform_dyn_yx_order_v vp_array_uniform_dyn_int_input_v
    vp_array_uniform_dyn_via_local_v vp_array_uniform_dyn_param_v
    vp_array_uniform_dyn_half4_v vp_array_uniform_dyn_float3_v
    vp_array_uniform_dyn_scalar_v vp_array_uniform_dyn_computed_v
    vp_array_uniform_dyn_work_between_v vp_array_uniform_dyn_float_index_v
    vp_array_uniform_dyn_half_index_v vp_array_uniform_dyn_float_expr_v)
if [[ -n "${PS3_REF_CG_COMPILER:-}" ]]; then
    [[ -x "$PS3_REF_CG_COMPILER" ]] || fail "PS3_REF_CG_COMPILER is not executable"
    for stem in $(grep -oE '^accept [a-z0-9_]+' "${BASH_SOURCE[0]}" | cut -d' ' -f2 | sort -u); do
        "$PS3_REF_CG_COMPILER" -p sce_vp_rsx -o "$work/$stem.ref.bin" "$shaders/$stem.cg" >"$work/$stem.ref.log" 2>&1 \
            || fail "reference: $stem did not compile"
        if cmp -s "$work/$stem.bin" "$work/$stem.ref.bin"; then status=identical; else status=differs; fi
        must=no; for b in "${BYTE_IDENTICAL[@]}"; do [[ "$b" == "$stem" ]] && must=yes; done
        [[ "$must" == yes && "$status" == differs ]] && fail "reference: $stem is listed byte-identical but differs"
        printf '  reference %-40s %s\n' "$stem" "$status"
    done
fi

printf 'PASS: vp-array-dynamic-test\n'
