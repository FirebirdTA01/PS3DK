#!/usr/bin/env bash
# An ARRAY out parameter, or an array member of an out struct parameter, is
# not stored yet: it must refuse by name (exit 1, no container).
#
# Why this is pinned: a semantic-less member of the returned struct takes
# the lowest TEXCOORD (vertex) or COLOR (fragment) index no output claims.
# The claim covers every element of an array output, but these shapes are
# refused before emission, so that path has no witness yet.  Measured on the
# reference, which accepts all three: the elements hold TEXCOORD0/1 (COLOR0/1)
# and the member goes to TEXCOORD2 (COLOR2).  When these shapes become
# supported, this test must turn into record and output-mask checks against
# those measurements instead of passing silently.
#
# Usage: tests/shader-compiler/array-out-param-refusal-test.sh <compiler>
set -u
compiler="${1:-${RSX_CG_COMPILER:-}}"
[ -n "$compiler" ] || { echo "array-out-param-refusal: FAIL: compiler required"; exit 1; }
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
fail=0
refuse() {   # <case> <profile> <source>
    printf '%s\n' "$3" > "$work/$1.cg"
    "$compiler" -p "$2" -e main --emit-container "$work/$1.bin" "$work/$1.cg" > "$work/$1.log" 2>&1
    local rc=$?
    if [ $rc -eq 1 ] && [ ! -s "$work/$1.bin" ] && grep -q "member-array-storage" "$work/$1.log"; then
        echo "array-out-param-refusal: ok   $1 refused by name"
    else
        echo "array-out-param-refusal: FAIL $1: rc $rc, want exit 1 with the member-array-storage refusal"; fail=1
    fi
}
refuse array_out_param sce_vp_rsx '
struct R { float4 p : POSITION; float4 a; };
R main(float4 v : POSITION, float4 t : TEXCOORD1, out float4 explicitTc[2] : TEXCOORD0) { R r; r.p = v; r.a = t * t; explicitTc[0] = t; explicitTc[1] = t.yxzw; return r; }'
refuse array_in_out_struct sce_vp_rsx '
struct O { float4 e[2] : TEXCOORD0; };
struct R { float4 p : POSITION; float4 a; };
R main(float4 v : POSITION, float4 t : TEXCOORD1, out O o) { R r; r.p = v; r.a = t * t; o.e[0] = t; o.e[1] = t.yxzw; return r; }'
refuse fp_array_out_param sce_fp_rsx '
struct F { float4 x; };
F main(float2 t : TEXCOORD0, out float4 c[2] : COLOR0) { F f; f.x = float4(t, t); c[0] = 1; c[1] = 0.5; return f; }'
[ $fail -eq 0 ] && echo "array-out-param-refusal: PASS" || echo "array-out-param-refusal: FAIL"
exit $fail
