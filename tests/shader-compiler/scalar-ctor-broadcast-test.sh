#!/usr/bin/env bash
# t_75de19a1: a vector constructor from ONE scalar is a broadcast.
#
#   float3(s)        float4(s)        float3(s, s, s)
#
# all lower to one MOV with the scalar replicated into every result lane.
# Measured on the reference: the single-scalar and spelled-out spellings
# compile to byte-identical containers (320 bytes for the .rgb member store,
# 272 for the float4 return), and the member store is `MOVR R0.xyz, R0.z`.
# Before this the single-scalar form fell into the general packer, whose
# operand-width sum (1) never matches a wider result, and refused - itself
# the honest replacement for a silent miscompile that dropped the store
# (t_c1d781ba).  This is the third and correct state.
#
# What is pinned, and why each half matters:
#   - the single-scalar spelling COMPILES (red on the parent);
#   - its bytes EQUAL an INDEPENDENT spelling's - the swizzle broadcast
#     (.bbb / .www), which never enters the vector constructor - and the
#     spelled-out float3(s, s, s)'s, which does and is therefore not proof;
#   - the broadcast MOV's DECODED destination mask and source swizzle are
#     the full result mask and the extracted lane replicated: a review found
#     spelling equality green under a forced lane-0 swizzle and under a
#     one-lane mask, because both spellings share the path;
#   - three DIFFERENT scalars are NOT folded, each lane reading its own;
#   - a repeated NON-scalar operand, float4(h, h) with a float2 h, is packed
#     lane-pair by lane-pair and never broadcast: the width == 1 test is the
#     boundary, and a review found it unguarded;
#   - a width mismatch (float3(float2, float2)) still refuses by name and
#     leaves no artifact.  The reference accepts that shape with warning
#     C7541 and truncates; our refusal is a known portability gap tracked
#     separately, and this row pins only that the broadcast path does not
#     swallow it.
# Shapes covered: struct-member .rgb store, float4 return, float2, and the
# vertex profile.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-scalar-ctor-broadcast.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT
shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"

emit() {  # <stem> -> exit status; container at $work/<stem>.fpo, log beside it
    local stem="$1"
    rm -f "$work/$stem.fpo"
    local rc=0
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-20s}" "$compiler" \
            -p sce_fp_rsx --emit-container "$work/$stem.fpo" "$shaders/$stem.cg"
    ) >"$work/$stem.log" 2>&1 || rc=$?
    [[ "$rc" -eq 124 ]] && fail "$stem timed out"
    printf '%s' "$rc"
}
accept() {  # <stem>
    local rc; rc="$(emit "$1")"
    [[ "$rc" -eq 0 ]] || { tail -n 3 "$work/$1.log" >&2; fail "$1 exited $rc, expected 0"; }
    [[ -s "$work/$1.fpo" ]] || fail "$1 compiled but wrote no container"
}

# insns <stem>: one line per INSTRUCTION, from the shared decoder
# (fp_sources.py, t_c83277c9).  This file used to carry its own, one of six
# in the tree; the shared one owns the on-disk half-swap, the inline-constant
# skip and the opcode-to-arity table, and it names an input source's VARYING
# instead of the register field - which is always zero for an input, so the
# `src=1:R0` this file used to match meant nothing.
#
#     <n> MOV dst=R<n> mask=<xyzw> prec=<p> sat=<s> end=<e> s0=<src>.<swz>
#
# TWO DIFFERENCES THAT MATTER TO THE ROWS BELOW.  The mask is rendered as
# LANE LETTERS rather than a hex nibble, and EVERY instruction is printed,
# not only the MOVs - so every count and every negative assertion here is
# scoped to `MOV`, or a TEX writing R0.xyz in some future fixture would
# satisfy a row that is about the broadcast.
insns() {
    python3 "$repo_root/tests/shader-compiler/fp_sources.py" "$work/$1.fpo"
}

# Two oracles for each shape, because a single one can be fooled:
#   1. BYTES against the SWIZZLE spelling (.bbb / .gggg), which never goes
#      through the vector constructor - so the constructor cannot break
#      both sides of that comparison at once.  The spelled-out constructor
#      float3(s, s, s) now shares the broadcast path and is compared too,
#      but it is not independent of it and is not the proof.
#   2. The broadcast MOV's DECODED FIELDS: the full result mask and the
#      replicated source lane the extract selected (.b -> zzzz, .g ->
#      yyyy).  A lowering that forced lane 0, or wrote one lane, or split
#      the store, fails here even if it did so in every spelling alike.
accept fp_scalar_ctor_explicit_f
accept fp_scalar_ctor_swizzle3_f
accept fp_scalar_ctor_broadcast_f
cmp -s "$work/fp_scalar_ctor_broadcast_f.fpo" "$work/fp_scalar_ctor_swizzle3_f.fpo" \
    || fail "float3(s) into a member .rgb store did not compile to the bytes of the .bbb swizzle spelling - the broadcast is not a broadcast"
cmp -s "$work/fp_scalar_ctor_broadcast_f.fpo" "$work/fp_scalar_ctor_explicit_f.fpo" \
    || fail "float3(s) and float3(s, s, s) compiled to different bytes"
insns fp_scalar_ctor_broadcast_f > "$work/bcast.insns"
grep -qE '^[0-9]+ MOV dst=R0 mask=xyz .* s0=R[0-9]+[.]zzzz$' "$work/bcast.insns" \
    || { cat "$work/bcast.insns" >&2; fail "the member store's broadcast MOV is not 'R0.xyz <- temp.zzzz': the scalar's lane (.b) is not what reaches all three lanes"; }
[[ "$(grep -cE '^[0-9]+ MOV dst=R0 mask=xyz ' "$work/bcast.insns")" == 1 ]] \
    || fail "expected exactly one xyz MOV into R0 for the member store"
printf '  %-36s == .bbb, MOV R0.xyz <- .zzzz\n' "float3(scalar) member store"

# The source remains the value at construction time even when its original
# vector is subsequently written. The saved lanes must not follow that write.
accept fp_scalar_ctor_snapshot_f
accept fp_scalar_ctor_snapshot_swizzle_f
cmp -s "$work/fp_scalar_ctor_snapshot_f.fpo" "$work/fp_scalar_ctor_snapshot_swizzle_f.fpo" \
    || fail "a broadcast snapshot changed when its source vector was subsequently written"
printf '  %-36s == saved .yyy before source write\n' "float3(scalar) snapshot"

accept fp_scalar_ctor_return4_explicit_f
accept fp_scalar_ctor_swizzle4_f
accept fp_scalar_ctor_return4_f
cmp -s "$work/fp_scalar_ctor_return4_f.fpo" "$work/fp_scalar_ctor_return4_explicit_f.fpo" \
    || fail "float4(s) on the return path did not compile to the bytes of float4(s, s, s, s)"
insns fp_scalar_ctor_return4_f > "$work/ret4.insns"
grep -qE '^[0-9]+ MOV dst=R[0-9]+ mask=xyzw .* s0=R[0-9]+[.]yyyy$' "$work/ret4.insns" \
    || { cat "$work/ret4.insns" >&2; fail "the float4(s) return has no xyzw MOV reading .yyyy - the scalar's lane (.g) is not what reaches all four lanes"; }
# The .gggg spelling's broadcast MOV has the same decoded fields; its bytes
# differ from float4(s) only by the extra return-store copy (t_e3af5f18),
# so the comparison here is on the decoded MOV, not on the container.
insns fp_scalar_ctor_swizzle4_f > "$work/swz4.insns"
grep -qE '^[0-9]+ MOV dst=R[0-9]+ mask=xyzw .* s0=R[0-9]+[.]yyyy$' "$work/swz4.insns" \
    || { cat "$work/swz4.insns" >&2; fail "the .gggg control lost its xyzw .yyyy MOV - the independent oracle for the return shape is broken"; }
printf '  %-36s == explicit, MOV xyzw <- .yyyy as .gggg\n' "float4(scalar) return"

# The two-lane shape, float2(s), which the parent refused.  Its bytes are
# one MOV longer than the .gg spelling's (the broadcast lands in a temp and
# the consumer copies it - the same uncoalesced shape as t_e3af5f18), so the
# pin is the decoded MOV: mask xy reading .yyyy.
accept fp_scalar_ctor_float2_swizzle_f
accept fp_scalar_ctor_float2_f
insns fp_scalar_ctor_float2_f > "$work/f2.insns"
grep -qE '^[0-9]+ MOV dst=R[0-9]+ mask=xy .* s0=R[0-9]+[.]yyyy$' "$work/f2.insns" \
    || { cat "$work/f2.insns" >&2; fail "float2(s) has no xy MOV reading .yyyy - the two-lane broadcast is wrong"; }
insns fp_scalar_ctor_float2_swizzle_f > "$work/f2s.insns"
grep -qE '^[0-9]+ MOV dst=R[0-9]+ mask=xy .* s0=R[0-9]+[.]yyyy$' "$work/f2s.insns" \
    || { cat "$work/f2s.insns" >&2; fail "the .gg control lost its xy .yyyy MOV - the independent oracle for float2 is broken"; }
printf '  %-36s MOV xy <- .yyyy as .gg\n' "float2(scalar)"

# The VERTEX profile takes the same path: float3(p.w) in a VP, which the
# parent refused, must compile to the bytes of the .www spelling (an
# independent path; the reference compiles both to identical 400-byte
# containers).  No VP word decoder here yet, so bytes carry this row.
vp_emit() {  # <stem> -> exit status, container at $work/<stem>.vpo
    local stem="$1"
    rm -f "$work/$stem.vpo"
    local rc=0
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-20s}" "$compiler" \
            -p sce_vp_rsx --emit-container "$work/$stem.vpo" "$shaders/$stem.cg"
    ) >"$work/$stem.log" 2>&1 || rc=$?
    [[ "$rc" -eq 124 ]] && fail "$stem timed out"
    printf '%s' "$rc"
}
for stem in vp_scalar_ctor_swizzle_v vp_scalar_ctor_broadcast_v; do
    rc="$(vp_emit "$stem")"
    [[ "$rc" -eq 0 ]] || { tail -n 3 "$work/$stem.log" >&2; fail "$stem exited $rc, expected 0"; }
    [[ -s "$work/$stem.vpo" ]] || fail "$stem compiled but wrote no container"
done
cmp -s "$work/vp_scalar_ctor_broadcast_v.vpo" "$work/vp_scalar_ctor_swizzle_v.vpo" \
    || fail "float3(p.w) in a vertex program did not compile to the bytes of the .www spelling"
printf '  %-36s == .www\n' "vertex float3(scalar)"

# Three different scalars stay three lanes, each reading its own.
accept fp_scalar_ctor_distinct_f
cmp -s "$work/fp_scalar_ctor_distinct_f.fpo" "$work/fp_scalar_ctor_broadcast_f.fpo" \
    && fail "float3(t.b, t.g, t.r) compiled to the BROADCAST's bytes - three different scalars were folded into one"
insns fp_scalar_ctor_distinct_f > "$work/distinct.insns"
for want in 'mask=x .* s0=R[0-9]+[.]zzzz' 'mask=y .* s0=R[0-9]+[.]yyyy' 'mask=z .* s0=R[0-9]+[.]xxxx'; do
    grep -qE "^[0-9]+ MOV dst=R0 $want\$" "$work/distinct.insns" \
        || { cat "$work/distinct.insns" >&2; fail "float3(t.b, t.g, t.r): missing the lane MOV '$want'"; }
done
printf '  %-36s packed: x<-.z y<-.y z<-.x\n' "float3(a, b, c)"

# The width BOUNDARY: a repeated NON-scalar operand is the packer's shape.
# float4(h, h) with a float2 h must pack lane pairs - xy <- .xyxx and
# zw <- .xxxy from the INPUT (source type 1) - and must NOT become one
# full-mask MOV: a broadcast keyed on operand identity alone would read
# .xyyy and put h.y into lane z (a review found exactly that mutation green
# before this row existed).  The reference merges the pair into one MOV
# reading .xyxy (swizzle 0x44); that merge is a separate packer gap
# (t_53f9b8ac), and when it lands this row's pins become that single MOV.
accept fp_scalar_ctor_pair2_f
insns fp_scalar_ctor_pair2_f > "$work/pair2.insns"
grep -qE '^[0-9]+ MOV dst=R0 mask=xy .* s0=TEX0[.]xyxx$' "$work/pair2.insns" \
    || { cat "$work/pair2.insns" >&2; fail "float4(h, h): missing the xy MOV reading the input's .xyxx"; }
grep -qE '^[0-9]+ MOV dst=R0 mask=zw .* s0=TEX0[.]xxxy$' "$work/pair2.insns" \
    || { cat "$work/pair2.insns" >&2; fail "float4(h, h): missing the zw MOV reading the input's .xxxy"; }
# MOV-scoped on purpose: the shared decoder prints EVERY instruction, so
# an unanchored 'mask=xyzw' would be satisfied by a TEX in some future
# version of this fixture and stop being about the broadcast at all.
grep -qE '^[0-9]+ MOV .* mask=xyzw ' "$work/pair2.insns" \
    && { cat "$work/pair2.insns" >&2; fail "float4(h, h) emitted a full-mask MOV - a width-2 operand was broadcast as if it were a scalar"; }
printf '  %-36s packed: xy<-.xy zw<-.xy, no broadcast\n' "float4(h, h), h float2"

# A width mismatch still refuses by name and leaves nothing behind.
rc="$(emit fp_scalar_ctor_width_refusal_f)"
[[ "$rc" -eq 1 ]] || fail "float3(float2, float2) exited $rc, expected exactly 1"
[[ ! -e "$work/fp_scalar_ctor_width_refusal_f.fpo" ]] || fail "the width refusal left an output artifact"
# Semantic analysis refuses this before lowering sees it, so the pinned body
# is the front end's, not the packer's.
grep -qF "constructor requires 3 components, but 4 were provided" "$work/fp_scalar_ctor_width_refusal_f.log" \
    || { head -n 3 "$work/fp_scalar_ctor_width_refusal_f.log" >&2; fail "the width mismatch was not refused by name"; }
printf '  %-36s refused, 4 components for 3\n' "float3(float2, float2)"

printf 'PASS: scalar-ctor-broadcast-test\n'
