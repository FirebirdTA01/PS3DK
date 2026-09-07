#!/usr/bin/env bash
# t_23f9d1a6: the Cg pack/unpack family - pack_2half, unpack_2half,
# pack_4ubyte, unpack_4ubyte, pack_4byte, unpack_4byte, pack_2ushort,
# unpack_2ushort - each ONE NV40 fragment instruction (PK2H/UP2H, PK4UB/UP4UB,
# PK4B/UP4B, PK2US/UP2US), as the reference emits them.  Sixteen reference-SDK
# fragment shaders (normal encoding, LogLuv, the post-processing utils) stopped
# at "no matching function for call to 'pack_2half(half2)'".
#
# MEASURED ON THE REFERENCE, 2026-09-07 (probes in C:/cgdev/pack-probe, one
# builtin per probe so a fact is attributable):
#
#   * A PACK reads its argument DIRECTLY - an input register, swizzled
#     (TEX0.yyyy for a scalar smear, TEX0.zwzw, TEX0.yxyx), or the temp an
#     expression or a texture fetch produced - and writes the lanes its
#     consumer reads (mask x when the result feeds an unpack or arithmetic).
#   * An UNPACK never reads an input register: an input argument is staged
#     through a MOV into a temp first (MOV R0.x <- TEX0; UP4UB R0 <- R0); a
#     temp produced by a pack, a MUL or a TEX is read directly.  Its
#     destination mask is the set of lanes the program consumes (xy for
#     .xy, yz for .gb, x for .x), and it is prec=0 even into a half local.
#   * Both profiles: only the FRAGMENT profile has them.  Under sce_vp_rsx
#     the reference refuses pack_2half with C1115 (no compatible overload)
#     and unpack_4ubyte with C5201 (invalid internal function declaration).
#   * Overloads: pack_2half takes half2 OR float2 and a float scalar smears
#     (.yyyy); a HALF scalar and a float3 are C1101 "ambiguous overloaded
#     function reference"; pack_4ubyte(float3) is C1115.
#   * When a pack's result goes STRAIGHT TO THE OUTPUT the reference stages
#     the argument through an H register first (MOV H0.xy <- TEX0 prec=1,
#     then PK2H R0.xyzw <- H0), and pack_2ushort through R (MOV R0.xy).  That
#     staging is the half-temp allocation of t_cde25bad, not this slice: the
#     output-direct rows below pin the pack instruction and its direct input
#     read and NAME the reference's extra MOV as the known byte difference.
#   * A pack result read by ARITHMETIC gets a FENCBR before the reader
#     (PK4UB R0.x; FENCBR; MUL) - an unpack reader gets none.  Pinned on the
#     tex_bgra row.
#   * BYTE IDENTITY: the out-parameter spelling of the normal-encoding shape
#     (fp_pack_normal_out_f) equals the reference byte for byte.  The
#     `return` spellings differ from it only by the export fold - the tip
#     ends every `return expr` with a MOV into R0 and keeps the chain in R1,
#     where the reference computes in R0 and ends on the last producer -
#     which is t_12bc176c's export-fold work, not this family; their
#     instructions, masks and swizzles are pinned here and they join
#     BYTE_IDENTICAL when that lands.
#
# CONTROL: every accept row fails on a compiler without the family ("no
# matching function for call to 'pack_2half(half2)'"), the refusal rows are
# checked against a stub compiler that accepts everything, and with
# PS3_REF_CG_COMPILER set the rows in BYTE_IDENTICAL must equal the
# reference's container byte for byte.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
here="$repo_root/tests/shader-compiler"
compiler="${1:-${RSX_CG_COMPILER:-}}"
[[ -n "$compiler" ]] || compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
[[ -x "$compiler" ]] || { printf 'FAIL: rsx-cg-compiler not executable: %s\n' "$compiler" >&2; exit 1; }
decoder="$here/fp_sources.py"
[[ -f "$decoder" ]] || { printf 'FAIL: fp_sources.py missing\n' >&2; exit 1; }
shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"

work="${TMPDIR:-/tmp}/ps3dk-pack-unpack-test.$$"
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
if ( refuse_with "$stub" "stub" vp_pack_refuse_v sce_vp_rsx "pack" ) >/dev/null 2>&1; then
    fail "self-check: refuse() passed against a compiler that accepts everything"
fi
pass "self-check: refuse() rejects an accept-all stub"

# ---------------------------------------------------------------- SDK shapes
# DeferredShading normal_encoding.cgh: pack two halves, unpack the bytes.
# The out-parameter spelling is byte-identical to the reference.
accept fp_pack_normal_out_f "normal encoding, out-parameter spelling (byte-identical)"
expect fp_pack_normal_out_f '^0 PK2H dst=R0 mask=x prec=0 sat=0 end=0 s0=TEX0\.xyzw$'
expect fp_pack_normal_out_f '^1 UP4UB dst=R0 mask=xyzw prec=0 sat=0 end=1 s0=R0\.xyzw$'
count  fp_pack_normal_out_f '^[0-9]+ ' 2
accept fp_pack_normal_f "pack_2half(float2) -> unpack_4ubyte, the normal-encoding shape"
expect fp_pack_normal_f '^[0-9]+ PK2H dst=R[0-9]+ mask=x .* s0=TEX0\.xyzw'
expect fp_pack_normal_f '^[0-9]+ UP4UB dst=R[0-9]+ mask=xyzw .* s0=R[0-9]+\.xyzw'
count  fp_pack_normal_f '^[0-9]+ (PK|UP)' 2

# LogLuv line 41: a SCALAR argument smears (.yyyy); the unpack writes only
# the two lanes the program consumes.
accept fp_pack_scalar_smear_f "pack_2half(scalar) smears the lane; unpack_4ubyte(...).xy"
expect fp_pack_scalar_smear_f '^[0-9]+ PK2H dst=R[0-9]+ mask=x .* s0=TEX0\.yyyy'
expect fp_pack_scalar_smear_f '^[0-9]+ UP4UB dst=R[0-9]+ mask=xy .* s0=R[0-9]+\.xyzw'

# LogLuv line 60: pack_4ubyte of a swizzled input, unpack_2half(...).x.
accept fp_pack_roundtrip_f "pack_4ubyte(v.zwzw) -> unpack_2half(...).x"
expect fp_pack_roundtrip_f '^[0-9]+ PK4UB dst=R[0-9]+ mask=x .* s0=TEX0\.zwzw'
expect fp_pack_roundtrip_f '^[0-9]+ UP2H dst=R[0-9]+ mask=x .* s0=R[0-9]+\.xyzw'

# post-sample utils.cg line 37 / 42.
accept fp_pack_utils_gb_f "pack_2half(ld.xx) -> unpack_4ubyte(...).gb"
expect fp_pack_utils_gb_f '^[0-9]+ PK2H dst=R[0-9]+ mask=x .* s0=TEX0\.xxxx'
expect fp_pack_utils_gb_f '^[0-9]+ UP4UB dst=R[0-9]+ mask=yz .* s0=R[0-9]+\.xyzw'
accept fp_pack_utils_yxyx_f "pack_4ubyte(pk.yxyx) -> unpack_2half(...).x"
expect fp_pack_utils_yxyx_f '^[0-9]+ PK4UB dst=R[0-9]+ mask=x .* s0=TEX0\.yxyx'
expect fp_pack_utils_yxyx_f '^[0-9]+ UP2H dst=R[0-9]+ mask=x '

# A texture fetch feeds the pack directly (post_fuzziness shape, tex2D form).
accept fp_pack_tex_f "pack_2half(tex2D(...).xy) reads the fetched temp directly"
expect fp_pack_tex_f '^[0-9]+ TEX '
expect fp_pack_tex_f '^[0-9]+ PK2H dst=R[0-9]+ mask=x .* s0=R[0-9]+\.xyzw'
expect fp_pack_tex_f '^[0-9]+ UP4UB dst=R[0-9]+ mask=xyzw '
accept fp_pack_tex_bgra_f "pack_4ubyte(tex2D(...).bgra) then arithmetic on the packed float"
expect fp_pack_tex_bgra_f '^[0-9]+ PK4UB dst=R[0-9]+ mask=x .* s0=R[0-9]+\.zyxw'
expect fp_pack_tex_bgra_f '^[0-9]+ MUL .* s0=R[0-9]+\.xxxx'
# The reference fences a pack result before an arithmetic reader, and only
# there: one FENCBR here, none in the pack -> unpack shapes above.
count  fp_pack_tex_bgra_f '^[0-9]+ FENCBR ' 1
count  fp_pack_normal_f '^[0-9]+ FENCBR ' 0
count  fp_pack_tex_f '^[0-9]+ FENCBR ' 0

# The rest of the family: unpack_4byte, unpack_2ushort, unpack_2half.
accept fp_pack_family_f "unpack_2half / unpack_4byte / unpack_2ushort in one program"
expect fp_pack_family_f '^[0-9]+ UP2H '
expect fp_pack_family_f '^[0-9]+ UP4B '
expect fp_pack_family_f '^[0-9]+ UP2US '
forbid fp_pack_family_f '^[0-9]+ UP[24][HBU]S? .* s0=TEX'

# An unpack never reads an input register: an input is staged through a MOV.
accept fp_unpack_input_f "unpack_4ubyte(input) stages the input through a temp"
expect fp_unpack_input_f '^[0-9]+ MOV dst=R[0-9]+ mask=x .* s0=TEX0\.'
expect fp_unpack_input_f '^[0-9]+ UP4UB dst=R[0-9]+ mask=xyzw .* s0=R[0-9]+\.xyzw'
forbid fp_unpack_input_f '^[0-9]+ UP4UB .* s0=TEX'
accept fp_unpack_expr_f "unpack_4ubyte(expression) reads the expression's temp directly"
expect fp_unpack_expr_f '^[0-9]+ MUL '
expect fp_unpack_expr_f '^[0-9]+ UP4UB dst=R[0-9]+ mask=xyzw .* s0=R[0-9]+\.xyzw'
count  fp_unpack_expr_f '^[0-9]+ MOV ' 1
# A declared half output: the reference writes UP4UB straight into H0
# (prec=0); the tip unpacks into a temp and converts at the store (MOV H0
# prec=1) - the same export fold as above.  Pinned: the unpack, and H0 as
# the register the colour reaches.
accept fp_unpack_half_out_f "unpack_4ubyte into a declared half output"
expect fp_unpack_half_out_f '^[0-9]+ UP4UB dst=R[0-9]+ mask=xyzw prec=0 '
expect fp_unpack_half_out_f '^[0-9]+ (UP4UB|MOV) dst=H0 mask=xyzw '

# ------------------------------------------------- output-direct pack shapes
# Pixel-correct today; the reference stages the argument through an H
# register first (MOV H0.xy <- TEX0 prec=1; PK2H R0.xyzw <- H0) - that MOV
# is t_cde25bad's half-temp allocation, so these rows pin the pack and its
# direct input read, and are NOT in BYTE_IDENTICAL until that lands.
# (The tip also keeps the .xxxx broadcast as a MOV after the pack where the
# reference folds it into the pack's own mask - the export fold again - so
# the pack's mask is x today and xyzw once that lands.)
accept fp_pack_return_f "pack_2half(float2) straight to the output"
expect fp_pack_return_f '^[0-9]+ PK2H dst=R0 mask=(x|xyzw) .* s0=(TEX0|H0)\.xyzw'
accept fp_pack_half2_return_f "pack_2half(half2) straight to the output"
expect fp_pack_half2_return_f '^[0-9]+ PK2H dst=R0 mask=(x|xyzw) '
accept fp_pack_2ushort_return_f "pack_2ushort(float2) straight to the output"
expect fp_pack_2ushort_return_f '^[0-9]+ PK2US dst=R0 mask=(x|xyzw) '
accept fp_pack_4byte_return_f "pack_4byte(float4) straight to the output"
expect fp_pack_4byte_return_f '^[0-9]+ PK4B dst=R0 mask=(x|xyzw) '

# ------------------------------------------------------------------ refusals
refuse "pack_2half under sce_vp_rsx (reference: C1115)"       vp_pack_refuse_v   sce_vp_rsx "pack_2half"
refuse "unpack_4ubyte under sce_vp_rsx (reference: C5201)"    vp_unpack_refuse_v sce_vp_rsx "unpack_4ubyte"
# A HALF scalar argument is C1101 "ambiguous overloaded function reference"
# in the reference (half -> half2 by smear ties with half -> float2 by smear
# and promotion); our overload resolution prefers the half2 overload and
# ACCEPTS it, smearing the lane as it does for a float scalar.  Pixel-safe
# and named rather than imitated: a resolver tie rule is t_37cc2ead's
# (stdlib overload parity), not this family's.
accept fp_pack_half_scalar_f "pack_2half(half scalar): accepted as a smear (reference: C1101 ambiguous)"
expect fp_pack_half_scalar_f '^[0-9]+ PK2H dst=R0 .* s0=TEX0\.xxxx'
refuse "pack_2half(float3) (reference: C1101 ambiguous)"      fp_pack_float3_refuse_f      sce_fp_rsx "pack_2half"
refuse "pack_4ubyte(float3) (reference: C1115)"               fp_pack4_float3_refuse_f     sce_fp_rsx "pack_4ubyte"

# ------------------------------------------------ reference byte identity
# The reference's executable is private (never named in the tree); with
# PS3_REF_CG_COMPILER set, these rows must match it byte for byte, and the
# remaining accept rows are reported (not asserted) so a newly identical row
# can be promoted here.
BYTE_IDENTICAL="fp_pack_normal_out_f"
if [[ -n "${PS3_REF_CG_COMPILER:-}" ]]; then
    [[ -x "$PS3_REF_CG_COMPILER" ]] || fail "PS3_REF_CG_COMPILER is not executable"
    # The half-scalar smear is the one accept row the reference REFUSES
    # (C1101); measured here rather than assumed.
    if "$PS3_REF_CG_COMPILER" -profile sce_fp_rsx -o "$work/half_scalar.ref" \
            "$shaders/fp_pack_half_scalar_f.cg" >/dev/null 2>&1; then
        fail "fp_pack_half_scalar_f: the reference now ACCEPTS pack_2half(half scalar); re-measure the overload rule"
    fi
    pass "fp_pack_half_scalar_f refused by the reference, as recorded"
    for stem in $(ls "$shaders" | grep -E '^fp_(un)?pack.*_f\.cg$' | grep -v -e refuse -e half_scalar | sed 's/\.cg$//'); do
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

printf 'pack-unpack-test: PASS\n'
