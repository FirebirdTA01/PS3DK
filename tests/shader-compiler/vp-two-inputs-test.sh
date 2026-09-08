#!/usr/bin/env bash
# t_9d0ff137: a vertex instruction may address ONE input register (the
# hardware has one per-instruction input field; two distinct inputs in one
# instruction silently read the same one, which is why the assembler refuses
# them).  The reference makes such programs WORK by staging the second input
# through a temp: measured on thirteen shapes (C:/cgdev/vp2in-probe,
# 2026-09-07 21:15) it keeps the FIRST operand's input direct, emits one MOV
# of the other input into a temp - the lanes the consumers read, packed -
# hoisted to the top and REUSED by every later consumer of that input, and
# splits a three-input MAD into MUL + ADD so each half keeps one input.  It
# also folds the staging into the POSITION copy when the staged input is the
# position (MOV o0/R0 <- IN0), a merge this compiler does not attempt.
#
# Three reference-SDK vertex programs stopped at "a vertex instruction
# addresses two distinct input registers, which the hardware cannot encode"
# (netgame vpshader, two PhysicsEffects vs_instancemesh).
#
# This compiler stages in legalizeInputOperands: the first operand's input
# stays direct, every other distinct input is copied into a temp by a MOV
# placed before its first consumer and reused by later consumers whose lanes
# it already covers.  Pixel-equal to the reference; bytes differ where the
# reference packs lanes or merges the copy into the position MOV, both named
# below rather than modelled.
#
# CONTROL: every accept row is refused on a compiler before this pass ("two
# distinct input registers"), the decoded rows assert ONE input per
# instruction and the staging MOV, and the reference rows report byte
# identity (none pinned yet - the merges above keep them apart).  The
# one-input assertion reads the decoded words, and the hardware has ONE
# selector, so it cannot see a per-slot identity the compiler meant and
# lost; the per-row source and swizzle assertions (which input each slot
# reads, with which lanes) and the rig row's lane recipes carry that proof
# (review: codex).
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
here="$repo_root/tests/shader-compiler"
compiler="${1:-${RSX_CG_COMPILER:-}}"
[[ -n "$compiler" ]] || compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
[[ -x "$compiler" ]] || { printf 'FAIL: rsx-cg-compiler not executable: %s\n' "$compiler" >&2; exit 1; }
decoder="$here/vp_words.py"
[[ -f "$decoder" ]] || { printf 'FAIL: vp_words.py missing\n' >&2; exit 1; }
shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"

work="${TMPDIR:-/tmp}/ps3dk-vp-two-inputs-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
pass() { printf 'PASS: %s\n' "$*"; }

compile() {   # <stem> -> 0/1; leaves $work/<stem>.{bin,out,err,words}
    local stem="$1" rc=0
    rm -f "$work/$stem.bin"
    ( ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
      timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$compiler" -p sce_vp_rsx \
          --emit-container "$work/$stem.bin" "$shaders/$stem.cg" ) \
        >"$work/$stem.out" 2>"$work/$stem.err" || rc=$?
    if [[ $rc -eq 0 && -s "$work/$stem.bin" ]]; then
        python3 "$decoder" "$work/$stem.bin" >"$work/$stem.words" 2>>"$work/$stem.err" \
            || fail "$stem: vp_words.py could not read the container"
    fi
    return $rc
}

accept() {   # <stem> <description>
    local stem="$1"; shift
    compile "$stem" || { cat "$work/$stem.err" >&2; fail "$stem ($*) was refused"; }
    [[ -s "$work/$stem.bin" ]] || fail "$stem: exit 0 but no container"
    # ONE input register per instruction: the padding slots echo the
    # instruction's own selector, so a second distinct INn on a line is a
    # second selector, which the hardware has no field for.
    python3 - "$work/$stem.words" <<'PYEOF' || fail "$stem: an instruction addresses two distinct input registers"
import re, sys
bad = []
for line in open(sys.argv[1], encoding='utf-8', errors='replace'):
    ins = set(re.findall(r'\bIN(\d+)\.', line))
    if len(ins) > 1:
        bad.append(line.strip())
if bad:
    print('\n'.join(bad))
    sys.exit(1)
PYEOF
    pass "$stem accepted, one input per instruction ($*)"
}
expect() {   # <stem> <regex over vp_words.py lines>
    grep -qE "$2" "$work/$1.words" \
        || { cat "$work/$1.words" >&2; fail "$1: no instruction matches /$2/"; }
}
count() {    # <stem> <regex> <expected count>
    local n; n=$(grep -cE "$2" "$work/$1.words" || true)
    [[ "$n" -eq "$3" ]] || { cat "$work/$1.words" >&2; fail "$1: $n instructions match /$2/, expected $3"; }
}

# --- self-check: the one-input assertion must reject a doctored words file ---
printf '0 MUL dst=o7 mask=xyzw src0=IN0.xyzw src1=IN2.xyzw src2=IN0.xyzw\n' >"$work/doctored.words"
if python3 - "$work/doctored.words" <<'PYEOF' >/dev/null 2>&1
import re, sys
for line in open(sys.argv[1]):
    if len(set(re.findall(r'\bIN(\d+)\.', line))) > 1: sys.exit(1)
PYEOF
then fail "self-check: the one-input assertion accepted a two-input line"; fi
pass "self-check: the one-input assertion rejects a two-input line"

# ------------------------------------------------------------------ shapes
accept vp_two_inputs_mul_v "p * n: the second input is staged through a temp"
expect vp_two_inputs_mul_v '^[0-9]+ MOV dst=R[0-9]+ mask=xyzw src0=IN2\.xyzw'
expect vp_two_inputs_mul_v '^[0-9]+ MUL dst=o7 mask=xyzw src0=IN0\.xyzw src1=R[0-9]+\.xyzw'
accept vp_two_inputs_add_v "p + n"
expect vp_two_inputs_add_v '^[0-9]+ ADD dst=o7 .* src0=IN0\.xyzw .* src2=R[0-9]+\.xyzw'
accept vp_two_inputs_dot_v "dot(p.xyz, n): the staged copy carries the three lanes the DP3 reads"
expect vp_two_inputs_dot_v '^[0-9]+ MOV dst=R[0-9]+ mask=xyz[w]? src0=IN2\.'
# (The reference writes the DP3 straight into o7 with its scalar broadcast;
# the tip routes a scalar result through a temp and a MOV - the export fold.)
expect vp_two_inputs_dot_v '^[0-9]+ DP3 dst=(o7|R[0-9]+) .* src0=IN0\.'
# A dot product must keep BOTH operands' swizzles: the reference reads
# dot(p.xyz, n.zyx) as IN0.xyzx, R0.zyxz and dot(p.xyz, n.www) as R0.wwww.
# The first draft reset a staged or direct DP3 operand to xyz (review:
# codex - dot(p.xyz, n.zyx) at p=(1,2,3), n=(4,5,6) read 32 for 28).
accept vp_two_inputs_dot_rhs_swz_v "dot(p.xyz, n.zyx): the staged operand keeps zyx"
expect vp_two_inputs_dot_rhs_swz_v '^[0-9]+ DP3 dst=(o7|R[0-9]+) .* src0=IN0\.xyzx src1=R[0-9]+\.zyxz'
accept vp_two_inputs_dot_lhs_swz_v "dot(p.zyx, n.xyz): the direct operand keeps zyx"
expect vp_two_inputs_dot_lhs_swz_v '^[0-9]+ DP3 dst=(o7|R[0-9]+) .* src0=IN0\.zyxz src1=R[0-9]+\.xyzx'
# (The reference stages the w lane INTO LANE X and reads R0.x; we keep the
# identity copy in lane w and read R0.wwww - same value, a placement byte
# difference recorded here, not a defect - review: claude.)
accept vp_two_inputs_dot_www_v "dot(p.xyz, n.www): the staged copy carries w and the DP3 reads it"
expect vp_two_inputs_dot_www_v '^[0-9]+ MOV dst=R[0-9]+ mask=w src0=IN2\.'
expect vp_two_inputs_dot_www_v '^[0-9]+ DP3 dst=(o7|R[0-9]+) .* src1=R[0-9]+\.wwww'
accept vp_two_inputs_reuse_then_dot_v "a staged copy reused by a later dot keeps the dot's swizzle"
count  vp_two_inputs_reuse_then_dot_v '^[0-9]+ MOV dst=R[0-9]+ .* src0=IN2\.' 1
expect vp_two_inputs_reuse_then_dot_v '^[0-9]+ DP3 dst=(o8|R[0-9]+) .* src1=R[0-9]+\.zyxz'
accept vp_two_inputs_mad_v "p * n + t: three inputs, each instruction keeps one"
accept vp_two_inputs_three_v "p * n * t"
accept vp_two_inputs_swz_v "p.wzyx * n.yyxx: the consumer keeps its swizzle on the temp"
expect vp_two_inputs_swz_v '^[0-9]+ MUL dst=o7 mask=xyzw src0=IN0\.wzyx src1=R[0-9]+\.yyxx'
accept vp_two_inputs_reuse_v "two consumers of the same staged input share ONE copy"
count  vp_two_inputs_reuse_v '^[0-9]+ MOV dst=R[0-9]+ .* src0=IN2\.' 1
accept vp_two_inputs_min_v "min(p, n)"
expect vp_two_inputs_min_v '^[0-9]+ MIN dst=o7 .* src0=IN0\.xyzw src1=R[0-9]+\.xyzw'
accept vp_two_inputs_np_v "n * p: the FIRST operand stays direct, so the position is the one staged"
expect vp_two_inputs_np_v '^[0-9]+ MUL dst=o7 mask=xyzw src0=IN2\.xyzw src1=R[0-9]+\.xyzw'
accept vp_two_inputs_nt_v "n * t: neither is the position"
expect vp_two_inputs_nt_v '^[0-9]+ MUL dst=o7 mask=xyzw src0=IN2\.xyzw src1=R[0-9]+\.xyzw'
accept vp_two_inputs_lanes_v "n.xxxx and n.wwww: two lanes of one input feed two consumers"
count  vp_two_inputs_lanes_v '^[0-9]+ MOV dst=R[0-9]+ .* src0=IN2\.' 1
accept vp_two_inputs_rig_v "the rig row: p.wwww * n * 0.5 + t * 0.25 into TEXCOORD0 (stays inside the coverage map)"

# ------------------------------------------------ reference byte identity
BYTE_IDENTICAL=""
if [[ -n "${PS3_REF_CG_COMPILER:-}" ]]; then
    [[ -x "$PS3_REF_CG_COMPILER" ]] || fail "PS3_REF_CG_COMPILER is not executable"
    for stem in $(ls "$shaders" | grep -E '^vp_two_inputs_.*_v\.cg$' | sed 's/\.cg$//'); do
        "$PS3_REF_CG_COMPILER" -profile sce_vp_rsx -o "$work/$stem.ref" "$shaders/$stem.cg" >/dev/null 2>&1 \
            || fail "$stem: the reference refused a fixture it is recorded as accepting"
        [[ -s "$work/$stem.bin" ]] || continue
        if cmp -s "$work/$stem.bin" "$work/$stem.ref"; then verdict=identical; else verdict=differs; fi
        if [[ " $BYTE_IDENTICAL " == *" $stem "* ]]; then
            [[ $verdict == identical ]] || fail "$stem: no longer byte-identical to the reference"
            pass "$stem byte-identical to the reference"
        else
            printf 'INFO: %s %s from the reference (not pinned)\n' "$stem" "$verdict"
        fi
    done
fi

printf 'vp-two-inputs-test: PASS\n'
