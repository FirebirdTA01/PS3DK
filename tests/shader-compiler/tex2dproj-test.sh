#!/usr/bin/env bash
# t_483feb71: tex2Dproj(sampler2D, float3|float4) - the projective texture
# fetch - is ONE NV40 instruction, TXP (opcode 0x18), as the reference emits
# it.  texCUBEproj stays refused in this slice (see its row).  Four reference-SDK fragment
# shaders (the two ShadowMapFp, Boy_FaceShadowFp, shadow_f) stopped at
# "nv40-general: unsupported IR op sampleproj".
#
# MEASURED ON THE REFERENCE, 2026-09-07 (probes C:/cgdev/txp-probe):
#   * TXP reads the coordinate exactly as TEX reads it - the input register
#     with its swizzle (TEX0.xyzw), or the temp an expression produced (MUL
#     R0; TXP R0 <- R0) - and writes the lanes the program consumes (mask x
#     when only .x is read); it divides x and y by the coordinate's LAST
#     lane, so the float3 spelling tex2Dproj(s, uv.xyw) is also TXP with
#     the identity read of the varying.
#   * A half output takes the fetch in R and converts at the store (TXP R0;
#     MOV H0 <- R0 prec=1) - byte-identical to the reference here.
#   * texCUBEproj is the same TXP on a cube sampler, with DISABLE_PC on the
#     instruction that reads the varying - kept REFUSED here (see the row).
#   * Two projective fetches from two samplers are two TXPs on two units.
#   * The reference ACCEPTS tex2Dproj in a vertex program (vertex texture
#     fetch); this compiler refuses every VP texture fetch by name and this
#     one keeps that refusal - measured as a divergence, not imitated.
#
# CONTROL: every accept row fails on a compiler before the TXP lowering
# ("unsupported IR op sampleproj"), the refusal row is checked against a
# stub that accepts everything, and with PS3_REF_CG_COMPILER set the rows
# in BYTE_IDENTICAL must equal the reference's container byte for byte.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
here="$repo_root/tests/shader-compiler"
compiler="${1:-${RSX_CG_COMPILER:-}}"
[[ -n "$compiler" ]] || compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
[[ -x "$compiler" ]] || { printf 'FAIL: rsx-cg-compiler not executable: %s\n' "$compiler" >&2; exit 1; }
decoder="$here/fp_sources.py"
[[ -f "$decoder" ]] || { printf 'FAIL: fp_sources.py missing\n' >&2; exit 1; }
shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"

work="${TMPDIR:-/tmp}/ps3dk-tex2dproj-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
pass() { printf 'PASS: %s\n' "$*"; }

# compile <compiler> <stem> <profile> -> 0/1; leaves $work/<stem>.{bin,out,err,srcs}
compile() {
    local cc="$1" stem="$2" profile="$3" rc=0
    rm -f "$work/$stem.bin"
    # stdout and stderr kept apart: a merged capture can splice a stderr line
    # into a dump row (the shared decoder refuses that; see ucode_decode.py).
    ( ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
      timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$cc" -p "$profile" \
          --emit-container "$work/$stem.bin" "$shaders/$stem.cg" ) \
        >"$work/$stem.out" 2>"$work/$stem.err" || rc=$?
    if [[ $rc -eq 0 && -s "$work/$stem.bin" ]]; then
        python3 "$decoder" "$work/$stem.bin" >"$work/$stem.srcs" 2>>"$work/$stem.err" \
            || fail "$stem: fp_sources.py could not read the container"
    fi
    return $rc
}

accept() {   # <stem> <description>
    local stem="$1"; shift
    compile "$compiler" "$stem" sce_fp_rsx \
        || { cat "$work/$stem.err" >&2; fail "$stem ($*) was refused"; }
    [[ -s "$work/$stem.bin" ]] || fail "$stem: exit 0 but no container"
    pass "$stem accepted ($*)"
}

expect() {   # <stem> <regex over fp_sources.py lines>
    grep -qE "$2" "$work/$1.srcs" \
        || { cat "$work/$1.srcs" >&2; fail "$1: no instruction matches /$2/"; }
}
forbid() {   # <stem> <regex that must match no line>
    if grep -qE "$2" "$work/$1.srcs"; then
        cat "$work/$1.srcs" >&2; fail "$1: an instruction matches /$2/, which the reference never emits"
    fi
}
count() {    # <stem> <regex> <expected count>
    local n; n=$(grep -cE "$2" "$work/$1.srcs" || true)
    [[ "$n" -eq "$3" ]] || { cat "$work/$1.srcs" >&2; fail "$1: $n instructions match /$2/, expected $3"; }
}

refuse() {   # <description> <stem> <profile> <message fragment>   (against $compiler)
    refuse_with "$compiler" "$@"
}
refuse_with() {   # <compiler> <description> <stem> <profile> <message fragment>
    local cc="$1" desc="$2" stem="$3" profile="$4" frag="$5" rc=0
    compile "$cc" "$stem" "$profile" || rc=$?
    [[ $rc -eq 1 ]] || { cat "$work/$stem.err" >&2; fail "$desc: exit $rc, expected the named refusal (exit 1)"; }
    [[ ! -e "$work/$stem.bin" ]] || fail "$desc: refused but left a container behind"
    grep -qF -- "$frag" "$work/$stem.err" \
        || { cat "$work/$stem.err" >&2; fail "$desc: refusal does not name '$frag'"; }
    pass "$desc refused by name"
}

# --- stub self-check: a compiler that accepts everything must FAIL refuse() ---
stub="$work/accept-all.sh"
printf '#!/usr/bin/env bash\nfor a in "$@"; do :; done\nout=""; while [[ $# -gt 0 ]]; do [[ $1 == --emit-container ]] && out=$2; shift; done\nprintf "x" > "$out"\nexit 0\n' >"$stub"
chmod +x "$stub"
if ( refuse_with "$stub" "stub" vp_txp_refuse_v sce_vp_rsx "texture" ) >/dev/null 2>&1; then
    fail "self-check: refuse() passed against a compiler that accepts everything"
fi
pass "self-check: refuse() rejects an accept-all stub"

# ---------------------------------------------------------------- shapes
accept fp_txp_proj4_out_f "tex2Dproj(float4) into an out parameter (byte-identical)"
expect fp_txp_proj4_out_f '^0 TXP dst=R0 mask=xyzw prec=0 sat=0 end=1 s0=TEX0\.xyzw$'
count  fp_txp_proj4_out_f '^[0-9]+ ' 1
accept fp_txp_proj4_f "tex2Dproj(float4) returned"
expect fp_txp_proj4_f '^[0-9]+ TXP dst=R[0-9]+ mask=xyzw .* s0=TEX0\.xyzw'
count  fp_txp_proj4_f '^[0-9]+ TXP ' 1
forbid fp_txp_proj4_f '^[0-9]+ TEX '
accept fp_txp_proj3_f "tex2Dproj(float3): divides by the coordinate's last lane"
expect fp_txp_proj3_f '^[0-9]+ TXP dst=R[0-9]+ mask=xyzw .* s0=TEX0\.'
# A float3 coordinate that lives in a TEMP or a CONSTANT has no w lane of
# its own: the hardware divides by source W, so the reference reads .xyzz
# (review: codex - the candidate read .xyzw and divided by an unwritten w,
# or by the literal block's zero).  The varying spellings already arrive
# with the last lane smeared; these three do not.
accept fp_txp_expr3_f "tex2Dproj(p.xyz * 2.0): the divisor is the temp's z"
expect fp_txp_expr3_f '^[0-9]+ TXP dst=R[0-9]+ mask=xyzw .* s0=R[0-9]+\.xyzz'
accept fp_txp_ctor3_f "tex2Dproj(float3(p.x, p.y, p.z)): the divisor is z"
expect fp_txp_ctor3_f '^[0-9]+ TXP dst=R[0-9]+ mask=xyzw .* s0=(TEX0|R[0-9]+)\.xyzz'
accept fp_txp_const3_f "tex2Dproj(float3(0.2, 0.4, 2.0)): divides by the constant's z, not the block's zero"
expect fp_txp_const3_f '^[0-9]+ TXP dst=R[0-9]+ mask=xyzw .* s0=c[0-9]+\.xyzz'
accept fp_txp_lane_f "tex2Dproj(...).x consumed as a scalar"
expect fp_txp_lane_f '^[0-9]+ TXP dst=R[0-9]+ mask=x .* s0=TEX0\.xyzw'
expect fp_txp_lane_f '^[0-9]+ MUL '
accept fp_txp_expr_f "tex2Dproj of an expression reads the producing temp"
expect fp_txp_expr_f '^[0-9]+ MUL '
expect fp_txp_expr_f '^[0-9]+ TXP dst=R[0-9]+ mask=xyzw .* s0=R[0-9]+\.xyzw'
accept fp_txp_half_out_f "tex2Dproj into a declared half output: fetch in R, convert at the store"
expect fp_txp_half_out_f '^[0-9]+ TXP dst=R[0-9]+ mask=xyzw prec=0 '
expect fp_txp_half_out_f '^[0-9]+ MOV dst=H0 mask=xyzw prec=1 '
accept fp_txp_shadow_f "the shadow-map shape: projective depth compared against z/w"
expect fp_txp_shadow_f '^[0-9]+ TXP dst=R[0-9]+ mask=x '
expect fp_txp_shadow_f '^[0-9]+ (DIV|RCP|SGT|SLT|SGE|SLE) '
accept fp_txp_two_f "two projective fetches on two samplers"
count  fp_txp_two_f '^[0-9]+ TXP ' 2
expect fp_txp_two_f '^[0-9]+ ADD '
# texCUBEproj stays REFUSED in this slice: a cube coordinate carries
# DISABLE_PC on the instruction that reads the varying feeding it (the fetch
# when it reads the input directly, the producing MOV/MUL/ADD when it does
# not, nothing for a constant), and the general path sets that bit nowhere -
# plain texCUBE has the same gap on the tip.  Measured by claude and codex
# (build/codex-cube-disable-pc-review, C:/cgdev/txp-probe/cube); one card
# for the propagation rule rather than a projective accept that ships the
# wrong bit.
refuse "texCUBEproj (reference: TXP with DISABLE_PC on the varying read)" fp_txp_cube_f sce_fp_rsx "texCUBEproj"

refuse "tex2Dproj under sce_vp_rsx (the reference accepts; every VP fetch refuses here by name)" vp_txp_refuse_v sce_vp_rsx "texture"

# ------------------------------------------------ reference byte identity
BYTE_IDENTICAL="fp_txp_proj4_out_f fp_txp_half_out_f"
if [[ -n "${PS3_REF_CG_COMPILER:-}" ]]; then
    [[ -x "$PS3_REF_CG_COMPILER" ]] || fail "PS3_REF_CG_COMPILER is not executable"
    for stem in $(ls "$shaders" | grep -E '^fp_txp_.*_f\.cg$' | grep -v cube | sed 's/\.cg$//'); do
        "$PS3_REF_CG_COMPILER" -profile sce_fp_rsx -o "$work/$stem.ref" "$shaders/$stem.cg" >/dev/null 2>&1 \
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

printf 'tex2dproj-test: PASS\n'
